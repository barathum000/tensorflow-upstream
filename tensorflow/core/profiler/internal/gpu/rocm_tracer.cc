/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/profiler/internal/gpu/rocm_tracer.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/node_hash_map.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/lib/hash/hash.h"
#include "tensorflow/core/platform/annotation.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mem.h"

#include "roctracer/roctracer.h"
#include "hip/hip_runtime.h"

namespace tensorflow {
namespace profiler {

namespace {

static thread_local bool internalRocmCall;

// Temporary disable cupti api tracing for this thread during the life scope of
// this class. Used for the API calls that initiated by us.
class RocmApiTracingDisabler {
 public:
  RocmApiTracingDisabler() { internalRocmCall = true; }
  ~RocmApiTracingDisabler() { internalRocmCall = false; }
};

Status ToStatus(roctracer_status_t result) {
  if (result == ROCTRACER_STATUS_SUCCESS) {
    return Status::OK();
  }
  const char *str = roctracer_error_string();
  return errors::Unavailable("ROCTRACER error: ", str ? str : "<unknown>");
}

Status ToStatus(hipError_t result) {
  if (result == hipSuccess) {
    return Status::OK();
  }
  const char *str = hipGetErrorString(result);
  return errors::Unavailable("ROCM error: ", str ? str : "<unknown>");
}

inline void LogIfError(const Status &status) {
  if (status.ok()) return;
  LOG(ERROR) << status.error_message();
}

#define RETURN_IF_ROCTRACER_ERROR(expr)                                     \
  do {                                                                      \
    roctracer_status_t status = expr;                                       \
    if (status != ROCTRACER_STATUS_SUCCESS) {                               \
      const char *errstr = roctracer_error_string();                        \
      LOG(ERROR) << "function " << #expr << "failed with error " << errstr; \
      return errors::Internal(absl::StrCat("roctracer call error", errstr));\
    }                                                                       \
  } while (false)

// GetCachedTID() caches the thread ID in thread-local storage (which is a
// userspace construct) to avoid unnecessary system calls. Without this caching,
// it can take roughly 98ns, while it takes roughly 1ns with this caching.
int32 GetCachedTID() {
  static thread_local int32 current_thread_id =
      Env::Default()->GetCurrentThreadId();
  return current_thread_id;
}

std::tuple<size_t /*bytes*/, RocmTracerEventType, bool /*async*/>
DecodeHipMemcpy(uint32_t cbid, const void *cbdata) {
  const hip_api_data_t *data = reinterpret_cast<const hip_api_data_t*>(cbdata);

  switch (cbid) {
    case HIP_API_ID_hipMemcpyDtoH: {
      return std::make_tuple(data->args.hipMemcpyDtoH.sizeBytes, RocmTracerEventType::MemcpyD2H, false);
    }
    case HIP_API_ID_hipMemcpyDtoHAsync: {
      return std::make_tuple(data->args.hipMemcpyDtoHAsync.sizeBytes, RocmTracerEventType::MemcpyD2H, true);
    }
    case HIP_API_ID_hipMemcpyHtoD: {
      return std::make_tuple(data->args.hipMemcpyHtoD.sizeBytes, RocmTracerEventType::MemcpyH2D, false);
    }
    case HIP_API_ID_hipMemcpyHtoDAsync: {
      return std::make_tuple(data->args.hipMemcpyHtoDAsync.sizeBytes, RocmTracerEventType::MemcpyH2D, true);
    }
    case HIP_API_ID_hipMemcpyDtoD: {
      return std::make_tuple(data->args.hipMemcpyDtoD.sizeBytes, RocmTracerEventType::MemcpyD2D, false);
    }
    case HIP_API_ID_hipMemcpyDtoDAsync: {
      return std::make_tuple(data->args.hipMemcpyDtoDAsync.sizeBytes, RocmTracerEventType::MemcpyD2D, true);
    }
    default: {
      LOG(ERROR) << "Unsupported memcpy activity observed: " << cbid;
      return std::make_tuple(0, RocmTracerEventType::Unsupported, false);
    }
  }
}

// Rocm callback corresponding to a driver or runtime API. This global function
// is invoked twice for each API: at entry and at exit. The cbdata
// parameter is guaranteed by Rocm to be thread-safe. Most invocations are
// dropped to the floor and entry/exit is tracked for the APIs we deem
// performance-relevant.
void ApiCallback(uint32_t domain,
                 uint32_t cbid,
                 const void *cbdata,
                 void *user_data) {
  RocmTracer *tracer = reinterpret_cast<RocmTracer *>(user_data);
  tracer->HandleCallback(domain, cbid, cbdata).IgnoreError();
}

void ActivityCallback(const char *begin, const char *end, void *user_data) {
  RocmTracer *tracer = reinterpret_cast<RocmTracer *>(user_data);
  tracer->ProcessActivityRecord(begin, end);
}

void AddKernelEventUponApiExit(RocmTraceCollector *collector, uint32 device_id,
                               const void *cbdata,
                               uint64 start_time, uint64 end_time) {
  const hip_api_data_t *data = reinterpret_cast<const hip_api_data_t*>(cbdata);
  const hipFunction_t kernelFunc = data->args.hipModuleLaunchKernel.f;
  RocmTracerEvent event;
  event.type = RocmTracerEventType::Kernel;
  event.source = RocmTracerEventSource::ApiCallback;
  if (kernelFunc != nullptr)
    event.name = hipKernelNameRef(kernelFunc);
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = GetCachedTID();
  event.device_id = device_id;
  event.correlation_id = data->correlation_id;

  event.kernel_info.dynamic_shared_memory_usage = data->args.hipModuleLaunchKernel.sharedMemBytes;
  event.kernel_info.block_x = data->args.hipModuleLaunchKernel.blockDimX;
  event.kernel_info.block_y = data->args.hipModuleLaunchKernel.blockDimY;
  event.kernel_info.block_z = data->args.hipModuleLaunchKernel.blockDimZ;
  event.kernel_info.grid_x = data->args.hipModuleLaunchKernel.gridDimX;
  event.kernel_info.grid_y = data->args.hipModuleLaunchKernel.gridDimY;
  event.kernel_info.grid_z = data->args.hipModuleLaunchKernel.gridDimZ;
 
  VLOG(3) << "HIP Kernel Launched: " << event.name;
  collector->AddEvent(std::move(event));
}

// Performs the actual callback for both normal and P2P memcpy operations.
RocmTracerEvent PopulateMemcpyCallbackEvent(
    RocmTracerEventType type, const void *cbdata,
    size_t num_bytes, uint32 src_device, uint32 dst_device, bool async,
    uint64 start_time, uint64 end_time) {
  const hip_api_data_t *data = reinterpret_cast<const hip_api_data_t*>(cbdata);
  RocmTracerEvent event;
  event.type = type;
  event.source = RocmTracerEventSource::ApiCallback;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = GetCachedTID();
  event.device_id = src_device;
  event.correlation_id = data->correlation_id;
  event.memcpy_info.num_bytes = num_bytes;
  event.memcpy_info.destination = dst_device;
  event.memcpy_info.async = async;
  return event;
}

void AddNormalMemcpyEventUponApiExit(RocmTraceCollector *collector,
                                     uint32 device_id, uint32_t cbid,
                                     const void *cbdata,
                                     uint64 start_time, uint64 end_time) {
  size_t num_bytes;
  RocmTracerEventType type;
  bool async;
  std::tie(num_bytes, type, async) =
      DecodeHipMemcpy(cbid, cbdata);

  VLOG(3) << "HIP Memcpy observed :" << num_bytes;
  RocmTracerEvent event =
      PopulateMemcpyCallbackEvent(type, cbdata, num_bytes, device_id, device_id,
                                  async, start_time, end_time);
  collector->AddEvent(std::move(event));
}

void AddMallocEventUponApiExit(RocmTraceCollector* collector, uint32 device_id,
                               uint32_t cbid, const void* cbdata,
                               uint64 start_time, uint64 end_time) {
  const hip_api_data_t* data = reinterpret_cast<const hip_api_data_t*>(cbdata);
  RocmTracerEvent event;
  event.name = roctracer_op_string(ACTIVITY_DOMAIN_HIP_API, cbid, 0);
  event.type = RocmTracerEventType::MemoryAlloc;
  event.source = RocmTracerEventSource::ApiCallback;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = GetCachedTID();
  event.device_id = device_id;
  event.correlation_id = data->correlation_id;
  switch (cbid) {
    case HIP_API_ID_hipMalloc:
      VLOG(3) << "HIP Malloc observed: " << data->args.hipMalloc.size;
      event.memalloc_info.num_bytes = data->args.hipMalloc.size;
      break;
    case HIP_API_ID_hipFree:
      VLOG(3) << "HIP Free observed";
      event.memalloc_info.num_bytes = 0;
      break;
  }
  collector->AddEvent(std::move(event));
}

void AddGenericEventUponApiExit(RocmTraceCollector *collector,
                                uint32 device_id, uint32_t cbid,
                                const void *cbdata,
                                uint64 start_time, uint64 end_time) {
  const hip_api_data_t *data = reinterpret_cast<const hip_api_data_t*>(cbdata);
  RocmTracerEvent event;
  event.name = roctracer_op_string(ACTIVITY_DOMAIN_HIP_API, cbid, 0);
  event.type = RocmTracerEventType::Generic;
  event.source = RocmTracerEventSource::ApiCallback;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = GetCachedTID();
  event.device_id = device_id;
  event.correlation_id = data->correlation_id;

  collector->AddEvent(std::move(event));
}

void AddKernelActivityEvent(RocmTraceCollector *collector,
                            AnnotationMap *annotation_map,
                            const void *data) {
  const roctracer_record_t *record = reinterpret_cast<const roctracer_record_t*>(data);

  RocmTracerEvent event;
  event.type = RocmTracerEventType::Kernel;
  event.source = RocmTracerEventSource::Activity;

  // ROCM TODO: need to populate these fields.
  event.name = ""; //kernel->name;
  event.start_time_ns = record->begin_ns;
  event.end_time_ns = record->end_ns;
  // ROCM TODO: need to populate these fields.
  event.device_id = 0; // record->device_id;
  event.stream_id = 0; // record->queue_id;
  event.correlation_id = record->correlation_id;
  event.annotation =
      annotation_map->LookUp(event.device_id, event.correlation_id);

  collector->AddEvent(std::move(event));
}

void AddMemcpyActivityEvent(RocmTraceCollector *collector,
                            AnnotationMap *annotation_map,
                            const void *data) {
  const roctracer_record_t *record = reinterpret_cast<const roctracer_record_t*>(data);

  RocmTracerEvent event;
  switch (record->op) {
    case HIP_API_ID_hipMemcpyDtoH:
      event.type = RocmTracerEventType::MemcpyD2H;
      event.name = "MemcpyD2H";
      event.memcpy_info.num_bytes = record->bytes;
      event.memcpy_info.async = false;
      // ROCM TODO: verify if this field is properly populated.
      event.memcpy_info.destination = record->device_id;
      break;
    case HIP_API_ID_hipMemcpyDtoHAsync:
      event.type = RocmTracerEventType::MemcpyD2H;
      event.name = "MemcpyD2H";
      event.memcpy_info.num_bytes = record->bytes;
      event.memcpy_info.async = true;
      // ROCM TODO: verify if this field is properly populated.
      event.memcpy_info.destination = record->device_id;
      break;
    case HIP_API_ID_hipMemcpyHtoD:
      event.type = RocmTracerEventType::MemcpyH2D;
      event.name = "MemcpyH2D";
      event.memcpy_info.num_bytes = record->bytes;
      event.memcpy_info.async = false;
      // ROCM TODO: verify if this field is properly populated.
      event.memcpy_info.destination = record->device_id;
      break;
    case HIP_API_ID_hipMemcpyHtoDAsync:
      event.type = RocmTracerEventType::MemcpyH2D;
      event.name = "MemcpyH2D";
      event.memcpy_info.num_bytes = record->bytes;
      event.memcpy_info.async = true;
      // ROCM TODO: verify if this field is properly populated.
      event.memcpy_info.destination = record->device_id;
      break;
    case HIP_API_ID_hipMemcpyDtoD:
      event.type = RocmTracerEventType::MemcpyD2D;
      event.name = "MemcpyD2D";
      event.memcpy_info.num_bytes = record->bytes;
      event.memcpy_info.async = false;
      // ROCM TODO: verify if this field is properly populated.
      event.memcpy_info.destination = record->device_id;
      break;
    case HIP_API_ID_hipMemcpyDtoDAsync:
      event.type = RocmTracerEventType::MemcpyD2D;
      event.name = "MemcpyD2D";
      event.memcpy_info.num_bytes = record->bytes;
      event.memcpy_info.async = true;
      // ROCM TODO: verify if this field is properly populated.
      event.memcpy_info.destination = record->device_id;
      break;
    default:
      event.type = RocmTracerEventType::MemcpyOther;
      event.name = "MemcpyOther";
      event.memcpy_info.num_bytes = record->bytes;
      event.memcpy_info.async = false;
      // ROCM TODO: verify if this field is properly populated.
      event.memcpy_info.destination = record->device_id;
      break;
  }
  event.source = RocmTracerEventSource::Activity;
  event.start_time_ns = record->begin_ns;
  event.end_time_ns = record->end_ns;

  // ROCM TODO: verify if these 2 fields are properly populated.
  event.device_id = record->device_id;
  event.stream_id = record->queue_id;

  event.correlation_id = record->correlation_id;
  event.annotation =
      annotation_map->LookUp(event.device_id, event.correlation_id);
  collector->AddEvent(std::move(event));
}

// This hook uses cupti activity api to measure device side activities.
class RocmDriverApiHookWithActivityApi : public RocmDriverApiHook {
 public:
  RocmDriverApiHookWithActivityApi(const RocmTracerOptions &option,
                                   RocmTraceCollector *collector,
                                   AnnotationMap *annotation_map)
      : option_(option),
        collector_(collector),
        annotation_map_(annotation_map) {}

  Status OnDriverApiEnter(int device_id, uint32_t domain,
                          uint32_t cbid,
                          const void *cbdata) override {
    return Status::OK();
  }
  Status OnDriverApiExit(int device_id, uint32_t domain,
                         uint32_t cbid,
                         const void *cbdata) override {
    // If we are not collecting CPU events from Callback API, we can return now.
    if (!option_.required_callback_api_events) {
      return Status::OK();
    }
    // ROCM TODO: revise this.
    // start_tsc and end_tsc are kept in activity logs.
    return AddDriverApiCallbackEvent(collector_, device_id,
                                     /*start_tsc*/0, /*end_tsc*/0, domain, cbid, cbdata);
  }
  Status Flush() override { return Status::OK(); }

 private:
  const RocmTracerOptions option_;
  RocmTraceCollector *collector_;
  AnnotationMap *annotation_map_;

  TF_DISALLOW_COPY_AND_ASSIGN(RocmDriverApiHookWithActivityApi);
};
}  // namespace

/*static*/ Status RocmDriverApiHook::AddDriverApiCallbackEvent(
    RocmTraceCollector *collector,
    int device_id, uint64 start_tsc, uint64 end_tsc,
    uint32_t domain, uint32_t cbid,
    const void *cbdata) {
  switch (cbid) {
    case HIP_API_ID_hipModuleLaunchKernel:
      AddKernelEventUponApiExit(collector, device_id, cbdata, start_tsc,
                                end_tsc);
      break;
    case HIP_API_ID_hipMemcpyDtoH:
    case HIP_API_ID_hipMemcpyDtoHAsync:
    case HIP_API_ID_hipMemcpyHtoD:
    case HIP_API_ID_hipMemcpyHtoDAsync:
    case HIP_API_ID_hipMemcpyDtoD:
    case HIP_API_ID_hipMemcpyDtoDAsync:
      AddNormalMemcpyEventUponApiExit(collector, device_id, cbid, cbdata,
                                      start_tsc, end_tsc);
      break;
    case HIP_API_ID_hipMalloc:
    case HIP_API_ID_hipFree:
      AddMallocEventUponApiExit(collector, device_id, cbid, cbdata, start_tsc,
                                end_tsc);
      break;
    default:
      AddGenericEventUponApiExit(collector, device_id, cbid, cbdata, start_tsc,
                                 end_tsc);
      break;
  }
  return Status::OK();
}

const char *GetTraceEventTypeName(const RocmTracerEventType &type) {
  switch (type) {
    case RocmTracerEventType::MemcpyH2D:
      return "MemcpyH2D";
    case RocmTracerEventType::MemcpyD2H:
      return "MemcpyD2H";
    case RocmTracerEventType::MemcpyD2D:
      return "MemcpyD2D";
    case RocmTracerEventType::MemcpyP2P:
      return "MemcpyP2P";
    case RocmTracerEventType::MemcpyOther:
      return "MemcpyOther";
    case RocmTracerEventType::Kernel:
      return "Compute";
    case RocmTracerEventType::MemoryAlloc:
      return "MemoryAlloc";
    case RocmTracerEventType::Generic:
      return "Generic";
    default:
      DCHECK(false);
      return "";
  }
}

void AnnotationMap::Add(uint32 device_id, uint32 correlation_id,
                        const std::string &annotation) {
  if (annotation.empty()) return;
  VLOG(3) << "Add annotation: device_id: " << device_id
          << " correlation_id: " << correlation_id
          << " annotation: " << annotation;
  if (device_id >= per_device_map_.size()) return;
  auto &per_device_map = per_device_map_[device_id];
  absl::MutexLock lock(&per_device_map.mutex);
  if (per_device_map.annotations.size() < max_size_) {
    absl::string_view annotation_str =
        *per_device_map.annotations.insert(annotation).first;
    per_device_map.correlation_map.emplace(correlation_id, annotation_str);
  }
}

absl::string_view AnnotationMap::LookUp(uint32 device_id,
                                        uint32 correlation_id) {
  if (device_id >= per_device_map_.size()) return absl::string_view();
  auto &per_device_map = per_device_map_[device_id];
  absl::MutexLock lock(&per_device_map.mutex);
  auto it = per_device_map.correlation_map.find(correlation_id);
  return it != per_device_map.correlation_map.end() ? it->second
                                                    : absl::string_view();
}

/* static */ RocmTracer *RocmTracer::GetRocmTracerSingleton() {
  static auto *singleton = new RocmTracer();
  return singleton;
}

bool RocmTracer::IsAvailable() const {
  return !activity_tracing_enabled_ && !api_tracing_enabled_;
}

int RocmTracer::NumGpus() {
  static int num_gpus = []() -> int {
    if (hipInit(0) != hipSuccess) {
      return 0;
    }
    int gpu_count;
    if (hipGetDeviceCount(&gpu_count) != hipSuccess) {
      return 0;
    }
    LOG(INFO) << "Profiler found " << gpu_count << " GPUs";
    return gpu_count;
  }();
  return num_gpus;
}

void RocmTracer::Enable(const RocmTracerOptions &option,
                        RocmTraceCollector *collector) {
  option_ = option;
  collector_ = collector;
  annotation_map_.emplace(option.max_annotation_strings, NumGpus());

  roctracer_driver_api_hook_.reset(new RocmDriverApiHookWithActivityApi(
      option, collector, &*annotation_map_));

  EnableApiTracing().IgnoreError();
  if (option_->enable_activity_api) {
    EnableActivityTracing().IgnoreError();
  }
}

void RocmTracer::Disable() {
  DisableApiTracing().IgnoreError();
  if (option_->enable_activity_api) {
    DisableActivityTracing().IgnoreError();
  }
  Finalize().IgnoreError();
  roctracer_driver_api_hook_->Flush().IgnoreError();
  collector_->Flush();
  collector_ = nullptr;
  option_.reset();
  roctracer_driver_api_hook_.reset();
  annotation_map_.reset();
}

Status RocmTracer::EnableApiTracing() {
  if (api_tracing_enabled_) return Status::OK();
  api_tracing_enabled_ = true;

  // ROCM TODO: do more selected filtering later on.
  //if (!option_->cbids_selected.empty()) {
  //  for (auto cbid : option_->cbids_selected) {
  //    // ROCM TODO: determine the best domain(s) to monitor.
  //    RETURN_IF_ROCTRACER_ERROR(roctracer_enable_op_callback(
  //        ACTIVITY_DOMAIN_HIP_API, cbid, ApiCallback, this));
  //  }
  //} else {  // select all callback ids.
    RETURN_IF_ROCTRACER_ERROR(roctracer_enable_callback(
        ApiCallback, this));
  //}
  return Status::OK();
}

Status RocmTracer::DisableApiTracing() {
  if (!api_tracing_enabled_) return Status::OK();

  api_tracing_enabled_ = false;

  if (!option_->cbids_selected.empty()) {
    for (auto cbid : option_->cbids_selected) {
      RETURN_IF_ROCTRACER_ERROR(roctracer_disable_op_callback(
          ACTIVITY_DOMAIN_HIP_API, cbid));
    }
  } else {
    RETURN_IF_ROCTRACER_ERROR(roctracer_disable_callback());
  }

  return Status::OK();
}

Status RocmTracer::EnableActivityTracing() {
  if (!option_->activities_selected.empty()) {
    // Initialize callback functions for Rocm Activity API.
    VLOG(1) << "Registering roctracer activity callbacks";

    // Creating tracer pool.
    roctracer_properties_t properties{};
    properties.buffer_size = 0x1000;
    properties.buffer_callback_fun = ActivityCallback;
    properties.buffer_callback_arg = this;
    if (roctracer_default_pool() == NULL)
      RETURN_IF_ROCTRACER_ERROR(roctracer_open_pool(&properties));

    VLOG(1) << "Enabling activity tracing for "
            << option_->activities_selected.size() << " activities";

    for (auto activity : option_->activities_selected) {
      VLOG(1) << "Enabling activity tracing for: " << activity;
      RETURN_IF_ROCTRACER_ERROR(roctracer_enable_domain_activity(activity));
    }
    //RETURN_IF_ROCTRACER_ERROR(roctracer_enable_activity());
  }
  activity_tracing_enabled_ = true;
  return Status::OK();
}

Status RocmTracer::DisableActivityTracing() {
  if (activity_tracing_enabled_) {
    VLOG(1) << "Disabling activity tracing for "
            << option_->activities_selected.size() << " activities";
    for (auto activity : option_->activities_selected) {
      VLOG(1) << "Disabling activity tracing for: " << activity;
      RETURN_IF_ROCTRACER_ERROR(roctracer_disable_domain_activity(activity));
    }
    //RETURN_IF_ROCTRACER_ERROR(roctracer_disable_activity());
    option_->activities_selected.clear();

    VLOG(1) << "Flushing roctracer activity buffer";
    RETURN_IF_ROCTRACER_ERROR(roctracer_flush_activity());
    LOG(INFO) << "roctracer activity buffer flushed";
  }
  activity_tracing_enabled_ = false;
  return Status::OK();
}

Status RocmTracer::Finalize() {
  return Status::OK();
}

/*static*/ uint64 RocmTracer::GetTimestamp() {
  uint64_t tsc;
  // ROCM TODO: revise with HIP or ROCR API
  //RocmInterface *cupti_interface = GetRocmInterface();
  //if (cupti_interface && cupti_interface->GetTimestamp(&tsc) == CUPTI_SUCCESS) {
  //  return tsc;
  //}
  // Return 0 on error. If an activity timestamp is 0, the activity will be
  // dropped during time normalization.
  return 0;
}

Status RocmTracer::HandleCallback(uint32_t domain, uint32_t cbid,
                                  const void *cbdata) {
  if (!api_tracing_enabled_) return Status::OK();  // already unsubscribed.
  if (domain != ACTIVITY_DOMAIN_HIP_API) return Status::OK();
  if (internalRocmCall) return Status::OK();

  // ROCM TODO: revise this.
  // Grab a correct device ID.
  uint32 device_id = 0;

  const hip_api_data_t *data = reinterpret_cast<const hip_api_data_t*>(cbdata);

  const char *name = roctracer_op_string(domain, cbid, 0);
  LOG(INFO) << "HIP API: " << name;
  LOG(INFO) << "domain: " << domain << " op: " << cbid << " correlation_id: " << data->correlation_id;

  if (data->phase == ACTIVITY_API_PHASE_ENTER) {
    TF_RETURN_IF_ERROR(roctracer_driver_api_hook_->OnDriverApiEnter(
        device_id, domain, cbid, cbdata));
  } else if (data->phase == ACTIVITY_API_PHASE_EXIT) {
    // Set up the map from correlation id to annotation string.
    const std::string &annotation = tensorflow::Annotation::CurrentAnnotation();
    if (!annotation.empty()) {
      annotation_map_->Add(device_id, data->correlation_id, annotation);
    }

    TF_RETURN_IF_ERROR(roctracer_driver_api_hook_->OnDriverApiExit(
        device_id, domain, cbid, cbdata));
  }
  return Status::OK();
}

Status RocmTracer::ProcessActivityRecord(const char *begin, const char *end) {
  if (!activity_tracing_enabled_) {
    LOG(WARNING) << "roctracer activity buffer is freed after flush.";
    return Status::OK();
  }

  const roctracer_record_t *record = reinterpret_cast<const roctracer_record_t*>(begin);
  const roctracer_record_t *end_record = reinterpret_cast<const roctracer_record_t*>(end);
  while (record < end_record) {
    const char *name = roctracer_op_string(record->domain, record->op, record->kind);
    LOG(INFO) << "activity: " << name;
    LOG(INFO) << "domain: " << record->domain << " op: " << record->op << " correlation_id: " << record->correlation_id << " begin_ns: " << record->begin_ns << " end_ns: " << record->end_ns;

    switch (record->op) {
      case HIP_API_ID_hipModuleLaunchKernel:
        AddKernelActivityEvent(collector_, &*annotation_map_, record);
        break;

      case HIP_API_ID_hipMemcpyDtoH:
      case HIP_API_ID_hipMemcpyHtoD:
      case HIP_API_ID_hipMemcpyDtoD:
      case HIP_API_ID_hipMemcpyDtoHAsync:
      case HIP_API_ID_hipMemcpyHtoDAsync:
      case HIP_API_ID_hipMemcpyDtoDAsync:
        AddMemcpyActivityEvent(collector_, &*annotation_map_, record);
        break;
    }

    RETURN_IF_ROCTRACER_ERROR(static_cast<roctracer_status_t>(roctracer_next_record(record, &record)));
  }
  return Status::OK();
}

}  // namespace profiler
}  // namespace tensorflow
