/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#include "xla/stream_executor/cuda/cuda_driver.h"

#include <stdint.h>
#include <stdlib.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "absl/base/casts.h"
#include "absl/base/const_init.h"
#include "absl/base/optimization.h"
#include "absl/container/inlined_vector.h"
#include "absl/debugging/leak_check.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"
#include "third_party/gpus/cuda/include/driver_types.h"
#include "xla/stream_executor/cuda/cuda_diagnostics.h"
#include "xla/stream_executor/gpu/gpu_driver.h"
#include "xla/stream_executor/platform/port.h"
#include "tsl/platform/env.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/stacktrace.h"
#include "tsl/platform/static_threadlocal.h"
#include "tsl/platform/status.h"
#include "tsl/platform/threadpool.h"

bool FLAGS_gpuexec_cuda_driver_inject_init_error = false;
bool FLAGS_gpuexec_cuda_sync_around_driver_calls = false;
bool FLAGS_gpuexec_cuda_device_0_only = false;

#define RETURN_IF_CUDA_RES_ERROR(expr, ...)                                   \
  do {                                                                        \
    CUresult _res = (expr);                                                   \
    if (ABSL_PREDICT_FALSE(_res != CUDA_SUCCESS)) {                           \
      if (_res == CUDA_ERROR_OUT_OF_MEMORY)                                   \
        return tsl::errors::ResourceExhausted(                                \
            __VA_ARGS__, ":", ::stream_executor::gpu::ToString(_res));        \
      else                                                                    \
        return tsl::errors::Internal(__VA_ARGS__, ": ",                       \
                                     ::stream_executor::gpu::ToString(_res)); \
    }                                                                         \
  } while (0)

#define FAIL_IF_CUDA_RES_ERROR(expr, ...)                   \
  do {                                                      \
    CUresult _res = (expr);                                 \
    if (ABSL_PREDICT_FALSE(_res != CUDA_SUCCESS)) {         \
      LOG(FATAL) << absl::StrCat(__VA_ARGS__) << ": "       \
                 << ::stream_executor::gpu::ToString(_res); \
    }                                                       \
  } while (0)

// Debugging: on each push and pop of a cuda context, verify the current context
// matches the expected one.
constexpr bool kVerifyGpuContext = false;

namespace stream_executor {
namespace gpu {

/* static */ absl::Mutex CreatedContexts::mu_{absl::kConstInit};
/* static */ int64_t CreatedContexts::next_id_ = 1;  // 0 means "no context"

namespace {

// Returns the current context and checks that it is in the set of CUDA contexts
// created by StreamExecutor (to ensure that the CUDA runtime didn't create a
// context behind our backs).
CUcontext CurrentContext() {
  CUcontext current = cuda::CurrentContextOrDie();
  if (current != nullptr && !CreatedContexts::Has(current)) {
    LOG(FATAL) << "current context was not created by the StreamExecutor "
                  "cuda_driver API: "
               << current
               << "; a CUDA runtime call "
                  "was likely performed without using a StreamExecutor context";
  }
  return current;
}

// CUDA driver routines may require a large amount of stack (particularly
// cuModuleLoadDataEx, in our experience). To avoid stack overflow when using
// stack-limited threads (such as those spawned by a default-argument
// thread::ThreadPool on some platforms), we run certain routines in this pool
// and wait for completion.
tsl::thread::ThreadPool* GetDriverExecutor() {
  static tsl::thread::ThreadPool* thread_pool = new tsl::thread::ThreadPool(
      tsl::Env::Default(), tsl::ThreadOptions(), "cuda_driver", 1);
  return thread_pool;
}

}  // namespace

std::string MemorySpaceString(MemorySpace memory_space) {
  switch (memory_space) {
    case MemorySpace::kHost:
      return "host";
    case MemorySpace::kDevice:
      return "device";
    default:
      LOG(FATAL) << "impossible memory space";
  }
}

namespace {

// Call cuCtxtSynchronize and crash if it doesn't succeed.
void SynchronizeOrDie() {
  FAIL_IF_CUDA_RES_ERROR(cuCtxSynchronize(),
                         "Synchronize fail: ", tsl::CurrentStackTrace());
}

struct ThreadLocalData {
  int64_t id;
  GpuContext* context;  // Only valid if id == a known good context.
  int depth;
};

TSL_STATIC_THREAD_LOCAL_POD(ThreadLocalData, tls_data);

}  // namespace

ScopedActivateContext::ScopedActivateContext(GpuContext* cuda_context) {
  if (FLAGS_gpuexec_cuda_sync_around_driver_calls) SynchronizeOrDie();

  auto* tls = &tls_data.get();

  // If this is an outermost scope, we must not assume that the CUDA context has
  // been left in the same state we left it. Other code may have run on this
  // thread and altered the context.
  if (tls->depth == 0) {
    VLOG(3) << "ScopedActivateContext switching to " << cuda_context->id();
    FAIL_IF_CUDA_RES_ERROR(cuCtxSetCurrent(cuda_context->context()),
                           "Failed setting context");
    tls->depth = 1;
    tls->id = cuda_context->id();
    tls->context = cuda_context;
    to_restore_ = nullptr;
    return;
  }

  tls->depth++;
  if (tls->id == cuda_context->id()) {
    if (kVerifyGpuContext) {
      CHECK_EQ(CurrentContext(), cuda_context->context());
    }
    DCHECK_EQ(CurrentContext(), cuda_context->context());
    return;
  }

  VLOG(3) << "ScopedActivateContext switching context from " << tls->id
          << " to " << cuda_context->id();

  to_restore_ = tls->context;
  // Set the context and update thread local.
  FAIL_IF_CUDA_RES_ERROR(cuCtxSetCurrent(cuda_context->context()),
                         "Failed setting context");
  tls->id = cuda_context->id();
  tls->context = cuda_context;
}

ScopedActivateContext::~ScopedActivateContext() {
  if (FLAGS_gpuexec_cuda_sync_around_driver_calls) SynchronizeOrDie();

  auto* tls = &tls_data.get();

  if (kVerifyGpuContext) {
    // Note that if kVerifyGpuContext is used, and contexts are deleted, it's
    // possible this could fail in the CurrentContext() call.
    CHECK_EQ(CurrentContext(),
             tls->context == nullptr ? nullptr : tls->context->context());
  }

  tls->depth--;
  DCHECK_GE(tls->depth, 0);
  if (to_restore_ == nullptr) {
    // Leave context, tls->id, and tls->context set.
    return;
  }

  // Set context and update thread local.
  FAIL_IF_CUDA_RES_ERROR(cuCtxSetCurrent(to_restore_->context()),
                         "Failed setting context");
  tls->id = to_restore_->id();
  tls->context = to_restore_;
}

namespace {

// Returns a stringified device number associated with pointer, primarily for
// logging purposes. Returns "?" if the device could not be successfully
// queried.
std::string CUDAPointerToDeviceString(CUdeviceptr pointer) {
  auto value = GpuDriver::GetPointerDevice(pointer);
  if (value.ok()) {
    return absl::StrCat(value.value());
  }
  LOG(ERROR) << "could not query device: " << value.status();
  return "?";
}

// Returns a stringified memory space associated with pointer, primarily for
// logging purposes. Returns "?" if the memory space could not be successfully
// queried.
std::string CUDAPointerToMemorySpaceString(CUdeviceptr pointer) {
  auto value = GpuDriver::GetPointerMemorySpace(pointer);
  if (value.ok()) {
    return MemorySpaceString(value.value());
  }
  LOG(ERROR) << "could not query device: " << value.status();
  return "?";
}

// Returns a stringified representation of whether or not peer access is
// permitted between the "from" and "to" pointers' associated contexts,
// primarily for logging purposes. Returns "error" if an error is encountered
// in the process of querying.
std::string CUDAPointersToCanAccessString(CUdeviceptr from, CUdeviceptr to) {
  auto from_context = GpuDriver::GetPointerContext(from);
  if (!from_context.ok()) {
    LOG(ERROR) << "could not retrieve source pointer's context: "
               << from_context.status();
    return "source ptr error";
  }
  auto to_context = GpuDriver::GetPointerContext(to);
  if (!to_context.ok()) {
    LOG(ERROR) << "could not retrieve destination pointer's context: "
               << to_context.status();
    return "destination ptr error";
  }
  return GpuDriver::CanEnablePeerAccess(from_context.value(),
                                        to_context.value())
             ? "true"
             : "false";
}

// Actually performs the work of CUDA initialization. Wrapped up in one-time
// execution guard.
static tsl::Status InternalInit() {
  CUresult res = CUDA_ERROR_NO_DEVICE;
  if (FLAGS_gpuexec_cuda_driver_inject_init_error) {
    LOG(ERROR) << "injecting CUDA init error; initialization will fail";
  } else {
    res = cuInit(0 /* = flags */);
  }

  if (res == CUDA_SUCCESS) {
    return ::tsl::OkStatus();
  } else if (res == CUDA_ERROR_SHARED_OBJECT_INIT_FAILED) {
    VLOG(1) << "failed call to cuInit: " << ToString(res);
  } else {
    LOG(ERROR) << "failed call to cuInit: " << ToString(res);
  }

  Diagnostician::LogDiagnosticInformation();
  return tsl::Status(absl::StatusCode::kAborted,
                     absl::StrCat("failed call to cuInit: ", ToString(res)));
}

}  // namespace

/* static */ tsl::Status GpuDriver::Init() {
  // Cached return value from calling InternalInit(), as cuInit need only be
  // called once, but GpuDriver::Init may be called many times.
  static tsl::Status* init_retval = [] {
    return new tsl::Status(InternalInit());
  }();
  return *init_retval;
}

/* static */ tsl::Status GpuDriver::GetDevice(int device_ordinal,
                                              CUdevice* device) {
  RETURN_IF_CUDA_RES_ERROR(cuDeviceGet(device, device_ordinal),
                           "Failed call to cuDeviceGet");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::GetDeviceName(CUdevice device,
                                                  std::string* device_name) {
  static const size_t kCharLimit = 64;
  absl::InlinedVector<char, 4> chars(kCharLimit);
  RETURN_IF_CUDA_RES_ERROR(
      cuDeviceGetName(chars.begin(), kCharLimit - 1, device),
      "Failed to get device name");
  chars[kCharLimit - 1] = '\0';
  *device_name = chars.begin();
  return ::tsl::OkStatus();
}

bool DeviceOptionsToContextFlags(const DeviceOptions& device_options,
                                 int* flags) {
  static_assert(DeviceOptions::kMask == 0xf,
                "needs update for new device options");

  if (device_options.flags() & DeviceOptions::kDoNotReclaimStackAllocation) {
    *flags |= CU_CTX_LMEM_RESIZE_TO_MAX;
  }

  // If no flags are set the default is CU_CTX_SCHED_AUTO, which
  // in Google environments is very likely to mean SPIN.
  if (device_options.flags() & DeviceOptions::kScheduleSpin) {
    *flags |= CU_CTX_SCHED_SPIN;
  }
  if (device_options.flags() & DeviceOptions::kScheduleYield) {
    *flags |= CU_CTX_SCHED_YIELD;
  }
  if (device_options.flags() & DeviceOptions::kScheduleBlockingSync) {
    *flags |= CU_CTX_SCHED_BLOCKING_SYNC;
  }

  return true;
}

/* static */ tsl::Status GpuDriver::CreateContext(
    int device_ordinal, CUdevice device, const DeviceOptions& device_options,
    GpuContext** context) {
  *context = nullptr;

  int flags = 0;
  if (!DeviceOptionsToContextFlags(device_options, &flags)) {
    LOG(WARNING) << "could not convert all device options into context flags";
  }

  CUresult res;
  CUcontext former_context;
  CUcontext new_context;

  unsigned int former_primary_context_flags;
  int former_primary_context_is_active;
  CHECK_EQ(CUDA_SUCCESS,
           cuDevicePrimaryCtxGetState(device, &former_primary_context_flags,
                                      &former_primary_context_is_active));
  if (former_primary_context_flags != flags) {
    if (former_primary_context_is_active) {
      LOG(ERROR)
          << "The primary context is active and has a different flag set ("
          << former_primary_context_flags << ") than the desired flag set ("
          << flags << ").";
    } else {
      CHECK_EQ(CUDA_SUCCESS, cuDevicePrimaryCtxSetFlags(device, flags));
    }
  }

  former_context = cuda::CurrentContextOrDie();
  res = cuDevicePrimaryCtxRetain(&new_context, device);
  if (former_context != nullptr) {
    CUdevice former_device;
    if (cuCtxGetDevice(&former_device) == CUDA_SUCCESS) {
      if (former_device == device) {
        if (former_context == new_context) {
          VLOG(2) << "The primary context " << former_context << " for device "
                  << device
                  << " exists before initializing the StreamExecutor.";
        } else {
          LOG(WARNING) << "A non-primary context " << former_context
                       << " for device " << device
                       << " exists before initializing the StreamExecutor. The "
                       << "primary context is now " << new_context << ". We "
                       << "haven't verified StreamExecutor works with that.";
        }
      }
    } else {
      LOG(ERROR) << "Failed to get the device of the current context "
                 << former_context;
    }
  }
  CHECK_EQ(CUDA_SUCCESS, cuCtxSetCurrent(former_context));

  if (res == CUDA_SUCCESS) {
    *context = CreatedContexts::Add(new_context, device_ordinal);
    CHECK(*context != nullptr)
        << "success in this call must entail non-null result";
    VLOG(2) << "created or reused context " << new_context
            << " for this thread";
    return ::tsl::OkStatus();
  }

  std::string message =
      "failed call to cuDevicePrimaryCtxRetain: " + ToString(res);
  if (res == CUDA_ERROR_OUT_OF_MEMORY) {
    uint64_t total_memory;
    if (GetDeviceTotalMemory(device, &total_memory)) {
      absl::StrAppend(&message, "; total memory reported: ", total_memory);
    } else {
      absl::StrAppend(&message, "; could not query total memory");
    }
  }

  return tsl::Status(absl::StatusCode::kInternal, message);
}

/* static */ void GpuDriver::DestroyContext(GpuContext* context) {
  if (context == nullptr) {
    return;
  }
  CUcontext former_context = CurrentContext();
  CUresult res = cuCtxSetCurrent(context->context());
  CUdevice device;
  cuCtxGetDevice(&device);
  cuCtxSetCurrent(former_context);

  res = cuDevicePrimaryCtxRelease(device);

  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to release CUDA context; leaking: " << ToString(res);
  }

  CreatedContexts::Remove(context->context());
}

/* static */ CUcontext GpuDriver::GetContextHandle(GpuContext* context) {
  return context->context();
}

/* static */ tsl::Status GpuDriver::FuncGetAttribute(
    CUfunction_attribute attribute, CUfunction func, int* attribute_value) {
  RETURN_IF_CUDA_RES_ERROR(cuFuncGetAttribute(attribute_value, attribute, func),
                           "Failed to query kernel attribute: ", attribute);
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::FuncSetCacheConfig(
    CUfunction function, CUfunc_cache cache_config) {
  RETURN_IF_CUDA_RES_ERROR(cuFuncSetCacheConfig(function, cache_config),
                           "Failed to set CUDA kernel cache config");
  return ::tsl::OkStatus();
}

/* static */ tsl::StatusOr<CUsharedconfig> GpuDriver::ContextGetSharedMemConfig(
    GpuContext* context) {
  CUsharedconfig shared_mem_config;
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(cuCtxGetSharedMemConfig(&shared_mem_config),
                           "Failed to get shared memory config");
  return shared_mem_config;
}

/* static */ tsl::Status GpuDriver::ContextSetSharedMemConfig(
    GpuContext* context, CUsharedconfig shared_mem_config) {
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(cuCtxSetSharedMemConfig(shared_mem_config),
                           "Failed to set shared memory config");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::CreateGraph(CUgraph* graph) {
  VLOG(2) << "Create new CUDA graph";
  RETURN_IF_CUDA_RES_ERROR(cuGraphCreate(graph, /*flags=*/0),
                           "Failed to create CUDA graph");
  VLOG(2) << "Created CUDA graph " << *graph;
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::DestroyGraph(CUgraph graph) {
  VLOG(2) << "Destroy CUDA graph " << graph;
  RETURN_IF_CUDA_RES_ERROR(cuGraphDestroy(graph),
                           "Failed to destroy CUDA graph");
  return ::tsl::OkStatus();
}

static std::string_view StreamCaptureModeToString(
    GpuDriver::StreamCaptureMode mode) {
  switch (mode) {
    case GpuDriver::StreamCaptureMode::kGlobal:
      return "global";
    case GpuDriver::StreamCaptureMode::kThreadLocal:
      return "threadlocal";
    case GpuDriver::StreamCaptureMode::kRelaxed:
      return "relaxed";
  }
}

/* static */ tsl::Status GpuDriver::StreamBeginCapture(CUstream stream,
                                                       StreamCaptureMode mode) {
  CUstreamCaptureMode cu_mode;
  switch (mode) {
    case StreamCaptureMode::kGlobal:
      cu_mode = CU_STREAM_CAPTURE_MODE_GLOBAL;
      break;
    case StreamCaptureMode::kThreadLocal:
      cu_mode = CU_STREAM_CAPTURE_MODE_THREAD_LOCAL;
      break;
    case StreamCaptureMode::kRelaxed:
      cu_mode = CU_STREAM_CAPTURE_MODE_RELAXED;
      break;
  }

  VLOG(2) << "Beging stream " << stream << " capture in "
          << StreamCaptureModeToString(mode) << " mode";
  RETURN_IF_CUDA_RES_ERROR(cuStreamBeginCapture(stream, cu_mode),
                           "Failed to begin stream capture");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::StreamEndCapture(CUstream stream,
                                                     CUgraph* graph) {
  VLOG(2) << "End stream " << stream << " capture";

  RETURN_IF_CUDA_RES_ERROR(cuStreamEndCapture(stream, graph),
                           "Failed to end stream capture");

  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::GraphInstantiate(
    CUgraphExec* exec, CUgraph graph, const GraphInstantiateFlags& flags) {
  VLOG(2) << "Instante CUDA executable graph from graph " << graph << " ("
          << "auto_free_on_launch=" << flags.auto_free_on_launch << ", "
          << "device_launch=" << flags.device_launch << ", "
          << "use_node_priority=" << flags.use_node_prirotiy << ", "
          << "upload=" << flags.upload << ")";

#if CUDA_VERSION >= 12000
  uint64_t cu_flags = 0;
  if (flags.auto_free_on_launch)
    cu_flags |= CUDA_GRAPH_INSTANTIATE_FLAG_AUTO_FREE_ON_LAUNCH;
  if (flags.use_node_prirotiy)
    cu_flags |= CUDA_GRAPH_INSTANTIATE_FLAG_USE_NODE_PRIORITY;
  if (flags.device_launch)
    cu_flags |= CUDA_GRAPH_INSTANTIATE_FLAG_DEVICE_LAUNCH;
  if (flags.upload) cu_flags |= CUDA_GRAPH_INSTANTIATE_FLAG_UPLOAD;

  RETURN_IF_CUDA_RES_ERROR(cuGraphInstantiate(exec, graph, cu_flags),
                           "Failed to instantiate CUDA graph");
#else
  RETURN_IF_CUDA_RES_ERROR(cuGraphInstantiate(exec, graph, nullptr, nullptr, 0),
                           "Failed to instantiate CUDA graph");
#endif  // CUDA_VERSION >= 12000

  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::GraphLaunch(CUgraphExec exec,
                                                CUstream stream) {
  VLOG(2) << "Launching CUDA executable graph " << exec << " on a stream "
          << stream;
  RETURN_IF_CUDA_RES_ERROR(cuGraphLaunch(exec, stream),
                           "Failed to launch CUDA graph");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::GraphExecUpdate(
    CUgraphExec exec, CUgraph graph, GraphExecUpdateResultInfo* result) {
  VLOG(2) << "Update CUDA graph executable " << exec << " with graph " << graph;

#if CUDA_VERSION >= 12000
  CUgraphExecUpdateResultInfo cu_result;
  memset(&cu_result, 0, sizeof(cu_result));
  CUresult err_code = cuGraphExecUpdate(exec, graph, &cu_result);
  auto cu_result_enum = cu_result.result;
  if (cu_result.errorFromNode) {
    result->error_from_node = cu_result.errorFromNode;
  }
  if (cu_result.errorNode) {
    result->error_node = cu_result.errorNode;
  }
#else
  CUgraphExecUpdateResult cu_result;
  CUresult err_code = cuGraphExecUpdate(exec, graph, nullptr, &cu_result);
  auto cu_result_enum = cu_result;
#endif  // CUDA_VERSION >= 12000

  switch (cu_result_enum) {
    case CU_GRAPH_EXEC_UPDATE_SUCCESS:
      result->result = GraphExecUpdateResult::kSuccess;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR:
      result->result = GraphExecUpdateResult::kError;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_TOPOLOGY_CHANGED:
      result->result = GraphExecUpdateResult::kTopologyChanged;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_NODE_TYPE_CHANGED:
      result->result = GraphExecUpdateResult::kNodeTypeChanged;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_FUNCTION_CHANGED:
      result->result = GraphExecUpdateResult::kFunctionChanged;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_PARAMETERS_CHANGED:
      result->result = GraphExecUpdateResult::kParametersChanged;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_NOT_SUPPORTED:
      result->result = GraphExecUpdateResult::kNotSupported;
      break;
    case CU_GRAPH_EXEC_UPDATE_ERROR_UNSUPPORTED_FUNCTION_CHANGE:
      result->result = GraphExecUpdateResult::kUnsupportedFunctionChange;
      break;

    case CU_GRAPH_EXEC_UPDATE_ERROR_ATTRIBUTES_CHANGED:
      result->result = GraphExecUpdateResult::kAttributesChanged;
      break;
  }

  RETURN_IF_CUDA_RES_ERROR(err_code, "Failed to update CUDA graph");
  return ::tsl::OkStatus();
}

/* static */ tsl::StatusOr<GpuDriver::GraphNodeType>
GpuDriver::GraphNodeGetType(CUgraphNode node) {
  CUgraphNodeType cu_node_type;
  memset(&cu_node_type, 0, sizeof(cu_node_type));
  RETURN_IF_CUDA_RES_ERROR(cuGraphNodeGetType(node, &cu_node_type),
                           "Failed to get CUDA graph node type");

  switch (cu_node_type) {
    case CU_GRAPH_NODE_TYPE_KERNEL:
      return GraphNodeType::kKernel;
    case CU_GRAPH_NODE_TYPE_MEMCPY:
      return GraphNodeType::kMemcpy;
    case CU_GRAPH_NODE_TYPE_MEMSET:
      return GraphNodeType::kMemset;
    case CU_GRAPH_NODE_TYPE_HOST:
      return GraphNodeType::kHost;
    case CU_GRAPH_NODE_TYPE_GRAPH:
      return GraphNodeType::kGraph;
    case CU_GRAPH_NODE_TYPE_EMPTY:
      return GraphNodeType::kEmpty;
    case CU_GRAPH_NODE_TYPE_WAIT_EVENT:
      return GraphNodeType::kWaitEvent;
    case CU_GRAPH_NODE_TYPE_EVENT_RECORD:
      return GraphNodeType::kEventRecord;
    case CU_GRAPH_NODE_TYPE_EXT_SEMAS_SIGNAL:
      return GraphNodeType::kExtSemasSignal;
    case CU_GRAPH_NODE_TYPE_EXT_SEMAS_WAIT:
      return GraphNodeType::kExtSemasWait;
    case CU_GRAPH_NODE_TYPE_MEM_ALLOC:
      return GraphNodeType::kMemAlloc;
    case CU_GRAPH_NODE_TYPE_MEM_FREE:
      return GraphNodeType::kMemFree;
    case CU_GRAPH_NODE_TYPE_BATCH_MEM_OP:
      return GraphNodeType::kBatchMemOp;
  }

  return tsl::Status(absl::StatusCode::kInternal,
                     "Invalid CUDA graph node type");
}

/* static */ tsl::Status GpuDriver::DestroyGraphExec(CUgraphExec exec) {
  VLOG(2) << "Destroying CUDA executable graph " << exec;
  RETURN_IF_CUDA_RES_ERROR(cuGraphExecDestroy(exec),
                           "Failed to destroy CUDA executable graph");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::GraphDebugDotPrint(CUgraph graph,
                                                       const char* path) {
#if CUDA_VERSION >= 12000
  VLOG(2) << "Print CUDA graph " << graph << " debug dot file to " << path;

  int flags = CU_GRAPH_DEBUG_DOT_FLAGS_VERBOSE;
  RETURN_IF_CUDA_RES_ERROR(cuGraphDebugDotPrint(graph, path, flags),
                           "Failed to print gpu graph debug file");

  if (VLOG_IS_ON(100)) {
    std::string data;
    if (tsl::ReadFileToString(tsl::Env::Default(), path, &data).ok()) {
      VLOG(200) << "CUDA graph " << graph << " debug file:\n" << data;
    } else {
      LOG(WARNING) << "failed to read gpu graph debug file " << path;
    }
  }
#endif  // CUDA_VERSION >= 12000

  return ::tsl::OkStatus();
}

/* static */ tsl::StatusOr<bool> GpuDriver::StreamIsCapturing(CUstream stream) {
  VLOG(2) << "Checking if stream " << stream << " is capturing";

  CUstreamCaptureStatus status;
  RETURN_IF_CUDA_RES_ERROR(cuStreamIsCapturing(stream, &status),
                           "Failed to check stream capturing status");

  return status == CU_STREAM_CAPTURE_STATUS_ACTIVE;
}

/* static */ tsl::Status GpuDriver::GraphAddKernelNode(
    CUgraphNode* node, CUgraph graph, absl::Span<CUgraphNode> deps,
    absl::string_view kernel_name, CUfunction function, unsigned int grid_dim_x,
    unsigned int grid_dim_y, unsigned int grid_dim_z, unsigned int block_dim_x,
    unsigned int block_dim_y, unsigned int block_dim_z,
    unsigned int shared_mem_bytes, void** kernel_params, void** extra) {
  VLOG(2) << "Add kernel node to a graph " << graph
          << "; kernel: " << kernel_name << "; gdx: " << grid_dim_x
          << " gdy: " << grid_dim_y << " gdz: " << grid_dim_z
          << " bdx: " << block_dim_x << " bdy: " << block_dim_y
          << " bdz: " << block_dim_z << "; shmem: " << shared_mem_bytes
          << "; deps: " << deps.size();

  CUDA_KERNEL_NODE_PARAMS params;
  memset(&params, 0, sizeof(params));

  params.func = function;
  params.gridDimX = grid_dim_x;
  params.gridDimY = grid_dim_y;
  params.gridDimZ = grid_dim_z;
  params.blockDimX = block_dim_x;
  params.blockDimY = block_dim_y;
  params.blockDimZ = block_dim_z;
  params.sharedMemBytes = shared_mem_bytes;
  params.kernelParams = kernel_params;
  params.extra = extra;

  if (shared_mem_bytes != 0) {
    RETURN_IF_CUDA_RES_ERROR(
        cuFuncSetAttribute(function,
                           CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
                           shared_mem_bytes),
        "Failed to set shared memory size");
  }

  RETURN_IF_CUDA_RES_ERROR(
      cuGraphAddKernelNode(node, graph, deps.data(), deps.size(), &params),
      "Failed to add kernel node to a CUDA graph");

  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::GraphAddMemcpyD2DNode(
    GpuContext* context, CUgraphNode* node, CUgraph graph,
    absl::Span<CUgraphNode> deps, CUdeviceptr gpu_dst, CUdeviceptr gpu_src,
    uint64_t size) {
  VLOG(2) << "Add memcpy d2d node to a graph " << graph
          << "; dst: " << reinterpret_cast<void*>(gpu_dst)
          << "; src: " << reinterpret_cast<void*>(gpu_src) << "; size: " << size
          << "; context: " << context->context() << "; deps: " << deps.size();

  CUDA_MEMCPY3D params;
  memset(&params, 0, sizeof(params));

  params.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  params.srcDevice = gpu_src;
  params.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  params.dstDevice = gpu_dst;
  params.WidthInBytes = size;
  params.Height = 1;
  params.Depth = 1;

  RETURN_IF_CUDA_RES_ERROR(
      cuGraphAddMemcpyNode(node, graph, deps.data(), deps.size(), &params,
                           context->context()),
      "Failed to add memcpy d2d node to a CUDA graph");

  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::LaunchKernel(
    GpuContext* context, absl::string_view kernel_name, CUfunction function,
    unsigned int grid_dim_x, unsigned int grid_dim_y, unsigned int grid_dim_z,
    unsigned int block_dim_x, unsigned int block_dim_y,
    unsigned int block_dim_z, unsigned int shared_mem_bytes, CUstream stream,
    void** kernel_params, void** extra) {
  ScopedActivateContext activation(context);
  VLOG(2) << "launching kernel: " << kernel_name << "; gdx: " << grid_dim_x
          << " gdy: " << grid_dim_y << " gdz: " << grid_dim_z
          << " bdx: " << block_dim_x << " bdy: " << block_dim_y
          << " bdz: " << block_dim_z;
  if (shared_mem_bytes != 0) {
    RETURN_IF_CUDA_RES_ERROR(
        cuFuncSetAttribute(function,
                           CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
                           shared_mem_bytes),
        "Failed to set shared memory size");
  }
  RETURN_IF_CUDA_RES_ERROR(
      cuLaunchKernel(function, grid_dim_x, grid_dim_y, grid_dim_z, block_dim_x,
                     block_dim_y, block_dim_z, shared_mem_bytes, stream,
                     kernel_params, extra),
      "Failed to launch CUDA kernel: ", kernel_name,
      " with block dimensions: ", block_dim_x, "x", block_dim_y, "x",
      block_dim_z, " and grid dimensions: ", grid_dim_x, "x", grid_dim_y, "x",
      grid_dim_z, " and shared memory size: ", shared_mem_bytes);
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::LoadCubin(GpuContext* context,
                                              const char* cubin_bytes,
                                              CUmodule* module) {
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(
      cuModuleLoadFatBinary(module, cubin_bytes),
      "Failed to load in-memory CUBIN (compiled for a different GPU?).");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::LoadPtx(GpuContext* context,
                                            const char* ptx_contents,
                                            CUmodule* module) {
  absl::Notification notification;
  tsl::Status ret = ::tsl::OkStatus();
  GetDriverExecutor()->Schedule([context, ptx_contents, module, &ret,
                                 &notification]() {
    ScopedActivateContext activation(context);
    void* ptx_data = const_cast<char*>(ptx_contents);
    static const unsigned int kLogBufferBytesLimit = 1024;
    unsigned int error_log_buffer_bytes = kLogBufferBytesLimit;
    unsigned int info_log_buffer_bytes = kLogBufferBytesLimit;
    absl::InlinedVector<char, 4> error_log_buffer(error_log_buffer_bytes);
    absl::InlinedVector<char, 4> info_log_buffer(info_log_buffer_bytes);
    bool log_verbose = true;
    CUjit_option options[] = {CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
                              CU_JIT_ERROR_LOG_BUFFER,
                              CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
                              CU_JIT_INFO_LOG_BUFFER, CU_JIT_LOG_VERBOSE};
    // Note that the driver API wants the contents of this values to be stored
    // in an array of void*s, so we coerce them accordingly.
    void* option_values[] = {
        absl::bit_cast<void*>(uintptr_t(error_log_buffer_bytes)),
        absl::bit_cast<void*>(error_log_buffer.data()),
        absl::bit_cast<void*>(uintptr_t(info_log_buffer_bytes)),
        absl::bit_cast<void*>(info_log_buffer.data()),
        absl::bit_cast<void*>(uintptr_t(log_verbose))};
    CHECK(TF_ARRAYSIZE(options) == TF_ARRAYSIZE(option_values));

    CUresult res;
    {
      // TODO(leary) Need to see if NVIDIA can expunge the leakiness in their
      // module loading: see http://b/13248943
      absl::LeakCheckDisabler disabler;
      res = cuModuleLoadDataEx(module, ptx_data, TF_ARRAYSIZE(options), options,
                               option_values);
    }

    // The PTX JIT mutates the values in the option values array to reflect the
    // size of the logs it output; now that we've made the call, read the values
    // back out.
    error_log_buffer_bytes = reinterpret_cast<uintptr_t>(option_values[0]);
    info_log_buffer_bytes = reinterpret_cast<uintptr_t>(option_values[2]);
    CHECK_LE(error_log_buffer_bytes, kLogBufferBytesLimit);
    CHECK_LE(info_log_buffer_bytes, kLogBufferBytesLimit);

    if (res != CUDA_SUCCESS) {
      LOG(ERROR) << "failed to load PTX text as a module: " << ToString(res);
      // As a precaution for null termination of the API-provided value, ensure
      // that at least the last byte is null.
      error_log_buffer[error_log_buffer_bytes ? error_log_buffer_bytes - 1
                                              : 0] = '\0';
      LOG(ERROR) << "error log buffer (" << error_log_buffer_bytes
                 << " bytes): " << error_log_buffer.data();
      ret = tsl::errors::Internal("Failed to load PTX text as a module: ",
                                  ToString(res));
      notification.Notify();
    }

    VLOG(3) << "PTX compilation info log (" << info_log_buffer_bytes
            << " bytes): " << info_log_buffer.data();
    VLOG(3) << "PTX compilation error log (" << error_log_buffer_bytes
            << " bytes): " << error_log_buffer.data();
    CHECK(module != nullptr);
    notification.Notify();
  });
  notification.WaitForNotification();

  return ret;
}

/* static */ tsl::Status GpuDriver::LoadHsaco(GpuContext* context,
                                              const char* hsaco_contents,
                                              CUmodule* module) {
  return tsl::errors::Internal(
      "Feature not supported on CUDA platform (LoadHsaco)");
}

/* static */ tsl::Status GpuDriver::SynchronousMemsetUint8(GpuContext* context,
                                                           CUdeviceptr location,
                                                           uint8_t value,
                                                           size_t size) {
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(cuMemsetD8(location, value, size),
                           "Failed to memset memory");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::SynchronousMemsetUint32(
    GpuContext* context, CUdeviceptr location, uint32_t value,
    size_t uint32_count) {
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(cuMemsetD32(location, value, uint32_count),
                           "Failed to memset memory");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::AsynchronousMemsetUint8(
    GpuContext* context, CUdeviceptr location, uint8_t value,
    size_t uint32_count, CUstream stream) {
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(
      cuMemsetD8Async(location, value, uint32_count, stream),
      "Failed to enqueue async memset operation");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::AsynchronousMemsetUint32(
    GpuContext* context, CUdeviceptr location, uint32_t value,
    size_t uint32_count, CUstream stream) {
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(
      cuMemsetD32Async(location, value, uint32_count, stream),
      "Failed to enqueue async memset operation");
  return ::tsl::OkStatus();
}

/* static */ bool GpuDriver::AddStreamCallback(GpuContext* context,
                                               CUstream stream,
                                               StreamCallback callback,
                                               void* data) {
  // Note: flags param is required to be zero according to CUDA 6.0.
  CUresult res = cuLaunchHostFunc(stream, callback, data);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "unable to add host callback: " << ToString(res);
    return false;
  }
  return true;
}

/* static */ tsl::Status GpuDriver::GetModuleFunction(GpuContext* context,
                                                      CUmodule module,
                                                      const char* kernel_name,
                                                      CUfunction* function) {
  ScopedActivateContext activated{context};
  CHECK(module != nullptr && kernel_name != nullptr);
  cudaError_t cuda_error = cudaPeekAtLastError();
  if (cuda_error != cudaSuccess) {
    return tsl::Status(
        absl::StatusCode::kInternal,
        absl::StrCat("There was an error before calling cuModuleGetFunction (",
                     cuda_error, "): ", cudaGetErrorName(cuda_error), " : ",
                     cudaGetErrorString(cuda_error)));
  }
  RETURN_IF_CUDA_RES_ERROR(cuModuleGetFunction(function, module, kernel_name),
                           "Failed to get module function");
  return tsl::OkStatus();
}

/* static */ bool GpuDriver::GetModuleSymbol(GpuContext* context,
                                             CUmodule module,
                                             const char* symbol_name,
                                             CUdeviceptr* dptr, size_t* bytes) {
  ScopedActivateContext activated{context};
  CHECK(module != nullptr && symbol_name != nullptr &&
        (dptr != nullptr || bytes != nullptr));
  CUresult res = cuModuleGetGlobal(dptr, bytes, module, symbol_name);
  if (res != CUDA_SUCCESS) {
    // symbol may not be found in the current module, but it may reside in
    // another module.
    VLOG(2) << "failed to get symbol \"" << symbol_name
            << "\" from module: " << ToString(res);
    return false;
  }

  return true;
}

/* static */ void GpuDriver::UnloadModule(GpuContext* context,
                                          CUmodule module) {
  ScopedActivateContext activated{context};
  CUresult res = cuModuleUnload(module);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to unload module " << module
               << "; leaking: " << ToString(res);
  }
}

/* static */ tsl::StatusOr<CUdevice> GpuDriver::DeviceFromContext(
    GpuContext* context) {
  ScopedActivateContext activated{context};
  CUdevice device = -1;
  CUresult result = cuCtxGetDevice(&device);
  if (result == CUDA_SUCCESS) {
    return device;
  }

  return tsl::Status(
      absl::StatusCode::kInternal,
      absl::StrCat("failed to get device for context: ", ToString(result)));
}

/* static */ bool GpuDriver::CreateStream(GpuContext* context, CUstream* stream,
                                          int priority) {
  ScopedActivateContext activated{context};
  CUresult res;
  // If the priority is 0, then use the previous api to create the stream with
  // the default priority for backward compatibility. Probably there is no
  // difference in using the new api call but leaving it as is for now.
  if (priority == 0) {
    res = cuStreamCreate(stream, CU_STREAM_NON_BLOCKING);
  } else {
    res = cuStreamCreateWithPriority(stream, CU_STREAM_NON_BLOCKING, priority);
  }
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "could not allocate CUDA stream for context "
               << context->context() << ": " << ToString(res);
    return false;
  }

  VLOG(2) << "successfully created stream " << *stream << " for context "
          << context->context() << " on thread";
  return true;
}

/* static */ void GpuDriver::DestroyStream(GpuContext* context,
                                           CUstream* stream) {
  if (*stream == nullptr) {
    return;
  }

  ScopedActivateContext activated{context};
  CUresult res = cuStreamDestroy(*stream);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to destroy CUDA stream for context "
               << context->context() << ": " << ToString(res);
  } else {
    VLOG(2) << "successfully destroyed stream " << *stream << " for context "
            << context->context();
    *stream = nullptr;
  }
}

/* static */ void* GpuDriver::DeviceAllocate(GpuContext* context,
                                             uint64_t bytes) {
  if (bytes == 0) {
    return nullptr;
  }

  ScopedActivateContext activated{context};
  CUdeviceptr result = 0;
  CUresult res = cuMemAlloc(&result, bytes);
  if (res != CUDA_SUCCESS) {
    // LOG(INFO) because this isn't always important to users (e.g. BFCAllocator
    // implements a retry if the first allocation fails).
    LOG(INFO) << "failed to allocate "
              << tsl::strings::HumanReadableNumBytes(bytes) << " (" << bytes
              << " bytes) from device: " << ToString(res);
    return nullptr;
  }
  void* ptr = reinterpret_cast<void*>(result);
  VLOG(2) << "allocated " << ptr << " for context " << context->context()
          << " of " << bytes << " bytes";
  return ptr;
}

/* static */ void GpuDriver::DeviceDeallocate(GpuContext* context,
                                              void* location) {
  ScopedActivateContext activation(context);
  CUdeviceptr pointer = absl::bit_cast<CUdeviceptr>(location);
  CUresult res = cuMemFree(pointer);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to free device memory at " << location
               << "; result: " << ToString(res);
  } else {
    VLOG(2) << "deallocated " << location << " for context "
            << context->context();
  }
}

/* static */ void* GpuDriver::UnifiedMemoryAllocate(GpuContext* context,
                                                    uint64_t bytes) {
  ScopedActivateContext activation(context);
  CUdeviceptr result = 0;
  // "Portable" memory is visible to all CUDA contexts. Safe for our use model.
  CUresult res = cuMemAllocManaged(&result, bytes, CU_MEM_ATTACH_GLOBAL);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to alloc " << bytes
               << " bytes unified memory; result: " << ToString(res);
    return nullptr;
  }
  void* ptr = reinterpret_cast<void*>(result);
  VLOG(2) << "allocated " << ptr << " for context " << context->context()
          << " of " << bytes << " bytes in unified memory";
  return ptr;
}

/* static */ void GpuDriver::UnifiedMemoryDeallocate(GpuContext* context,
                                                     void* location) {
  ScopedActivateContext activation(context);
  CUdeviceptr pointer = absl::bit_cast<CUdeviceptr>(location);
  CUresult res = cuMemFree(pointer);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to free unified memory at " << location
               << "; result: " << ToString(res);
  } else {
    VLOG(2) << "deallocated unified memory at " << location << " for context "
            << context->context();
  }
}

/* static */ void* GpuDriver::HostAllocate(GpuContext* context,
                                           uint64_t bytes) {
  ScopedActivateContext activation(context);
  void* host_mem = nullptr;
  // "Portable" memory is visible to all CUDA contexts. Safe for our use model.
  CUresult res = cuMemHostAlloc(&host_mem, bytes, CU_MEMHOSTALLOC_PORTABLE);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to alloc " << bytes
               << " bytes on host: " << ToString(res);
  }
  return host_mem;
}

/* static */ void GpuDriver::HostDeallocate(GpuContext* context,
                                            void* location) {
  ScopedActivateContext activation(context);
  CUresult res = cuMemFreeHost(location);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "error deallocating host memory at " << location << ": "
               << ToString(res);
  }
}

/* static */ bool GpuDriver::HostRegister(GpuContext* context, void* location,
                                          uint64_t bytes) {
  ScopedActivateContext activation(context);
  // "Portable" memory is visible to all CUDA contexts. Safe for our use model.
  CUresult res =
      cuMemHostRegister(location, bytes, CU_MEMHOSTREGISTER_PORTABLE);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "error registering host memory at " << location << ": "
               << ToString(res);
    return false;
  }
  return true;
}

/* static */ bool GpuDriver::HostUnregister(GpuContext* context,
                                            void* location) {
  ScopedActivateContext activation(context);
  CUresult res = cuMemHostUnregister(location);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "error unregistering host memory at " << location << ": "
               << ToString(res);
    return false;
  }
  return true;
}

/* static */ int GpuDriver::GetGpuStreamPriority(
    GpuContext* context, stream_executor::StreamPriority stream_priority) {
  ScopedActivateContext activation(context);
  if (stream_priority == stream_executor::StreamPriority::Default) {
    return 0;
  }
  int lowest, highest;
  CUresult res = cuCtxGetStreamPriorityRange(&lowest, &highest);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR)
        << "Could not query stream priority range. Returning default priority.";
    return 0;
  }
  return stream_priority == stream_executor::StreamPriority::Highest ? highest
                                                                     : lowest;
}

#if CUDA_VERSION >= 10020
/* static */ tsl::StatusOr<GpuDriver::VmemSpan> GpuDriver::ReserveVirtualMemory(
    GpuContext* context, uint64_t bytes) {
  ScopedActivateContext activation(context);
  CUdeviceptr base;
  CUresult res = cuMemAddressReserve(&base, bytes, /*alignment=*/0,
                                     /*addr=*/0, /*flags=*/0);
  if (res != CUDA_SUCCESS) {
    return tsl::errors::Internal(
        absl::StrFormat("error reserving %d bytes of virtual GPU memory: %s",
                        bytes, ToString(res)));
  }
  return {{base, bytes}};
}

/* static */ void GpuDriver::FreeVirtualMemory(
    GpuContext* context, GpuDriver::VmemSpan reservation) {
  ScopedActivateContext activation(context);
  CUresult res = cuMemAddressFree(reservation.base, reservation.size_bytes);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "error freeing vmem reservation of size "
               << reservation.size_bytes << " at address " << reservation.base;
  }
}

/* static */ tsl::StatusOr<uint64_t> GpuDriver::GetMinAllocationGranularity(
    GpuDeviceHandle device) {
  CUmemAllocationProp props = {};
  props.type = CU_MEM_ALLOCATION_TYPE_PINNED;
  props.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
  props.location.id = device;

  size_t granularity;
  CUresult res = cuMemGetAllocationGranularity(
      &granularity, &props, CU_MEM_ALLOC_GRANULARITY_MINIMUM);
  if (res != CUDA_SUCCESS) {
    return tsl::errors::Internal("failed to get min allocation granularity: ",
                                 ToString(res));
  }
  return granularity;
}

/* static */ tsl::StatusOr<GpuDriver::GenericMemoryHandle>
GpuDriver::CreateMemoryHandle(GpuContext* context, uint64_t bytes) {
  ScopedActivateContext activation(context);
  auto device = DeviceFromContext(context);
  if (!device.ok()) {
    LOG(ERROR) << "Failed to get device from context" << device.status();
    return device.status();
  }

  CUmemAllocationProp props = {};
  props.type = CU_MEM_ALLOCATION_TYPE_PINNED;
  props.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
  props.location.id = device.value();

  CUmemGenericAllocationHandle mem_handle;
  CUresult res = cuMemCreate(&mem_handle, bytes, &props, 0);
  if (res != CUDA_SUCCESS) {
    return tsl::errors::Internal(
        absl::StrFormat("failed to create memory allocation of size %d: %s",
                        bytes, ToString(res)));
  }
  return GpuDriver::GenericMemoryHandle{mem_handle, bytes};
}

/* static */ void GpuDriver::ReleaseMemoryHandle(
    GpuContext* context, GpuDriver::GenericMemoryHandle handle) {
  ScopedActivateContext activation(context);

  CUresult res = cuMemRelease(handle.handle);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "Failed to release memory handle " << handle.handle
               << " of size " << handle.bytes << ": " << ToString(res);
  }
}

/* static */ tsl::Status GpuDriver::MapMemory(
    GpuContext* context, CUdeviceptr va,
    const GpuDriver::GenericMemoryHandle& handle,
    const std::vector<GpuDeviceHandle>& device_handles) {
  ScopedActivateContext activation(context);

  auto device = DeviceFromContext(context);
  if (!device.ok()) {
    return device.status();
  }

  // NB: Zero is the only valid value for both flags and offset.
  CUresult res =
      cuMemMap(va, handle.bytes, /*offset=*/0, handle.handle, /*flags=*/0);
  if (res != CUDA_SUCCESS) {
    return tsl::errors::Internal(absl::StrFormat(
        "Failed to map %d bytes at %d: %s", handle.bytes, va, ToString(res)));
  }

  std::vector<CUmemAccessDesc> access_descriptors(device_handles.size());
  for (int i = 0; i < access_descriptors.size(); ++i) {
    access_descriptors[i].location.id = device_handles[i];
    access_descriptors[i].location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    access_descriptors[i].flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
  }

  res = cuMemSetAccess(va, handle.bytes, access_descriptors.data(),
                       access_descriptors.size());
  if (res != CUDA_SUCCESS) {
    // Unmap the memory that we failed to set access for.
    if (cuMemUnmap(va, handle.bytes) != CUDA_SUCCESS) {
      LOG(ERROR)
          << "Failed to unmap memory in GpuDriver::MapMemory error path.";
    }
    return tsl::errors::Internal(absl::StrFormat(
        "Failed to set read/write access on memory mapped at %d: %s", va,
        ToString(res)));
  }
  return tsl::OkStatus();
}

/* static */ void GpuDriver::UnmapMemory(GpuContext* context, CUdeviceptr va,
                                         uint64_t bytes) {
  ScopedActivateContext activation(context);

  CUresult res = cuMemUnmap(va, bytes);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "Failed to unmap memory at " << va << " of size " << bytes
               << ": " << ToString(res);
  }
}

#endif

/* static */ tsl::Status GpuDriver::DestroyEvent(GpuContext* context,
                                                 CUevent* event) {
  if (*event == nullptr) {
    return tsl::Status(absl::StatusCode::kInvalidArgument,
                       "input event cannot be null");
  }

  ScopedActivateContext activated{context};
  RETURN_IF_CUDA_RES_ERROR(cuEventDestroy(*event),
                           "Error destroying CUDA event");
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::RecordEvent(GpuContext* context,
                                                CUevent event,
                                                CUstream stream) {
  ScopedActivateContext activated{context};
  RETURN_IF_CUDA_RES_ERROR(cuEventRecord(event, stream),
                           "Error recording CUDA event");
  return ::tsl::OkStatus();
}

/* static */ tsl::StatusOr<CUresult> GpuDriver::QueryEvent(GpuContext* context,
                                                           CUevent event) {
  ScopedActivateContext activated{context};
  CUresult res = cuEventQuery(event);
  if (res != CUDA_SUCCESS && res != CUDA_ERROR_NOT_READY) {
    return tsl::Status(
        absl::StatusCode::kInternal,
        absl::StrFormat("failed to query event: %s", ToString(res)));
  }

  return res;
}

/* static */ bool GpuDriver::GetEventElapsedTime(GpuContext* context,
                                                 float* elapsed_milliseconds,
                                                 CUevent start, CUevent stop) {
  ScopedActivateContext activated{context};
  // The stop event must have completed in order for cuEventElapsedTime to
  // work.
  CUresult res = cuEventSynchronize(stop);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to synchronize the stop event: " << ToString(res);
    return false;
  }
  res = cuEventElapsedTime(elapsed_milliseconds, start, stop);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to get elapsed time between events: "
               << ToString(res);
    return false;
  }

  return true;
}

/* static */ bool GpuDriver::WaitStreamOnEvent(GpuContext* context,
                                               CUstream stream, CUevent event) {
  ScopedActivateContext activation(context);
  CUresult res = cuStreamWaitEvent(stream, event, 0 /* = flags */);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "could not wait stream on event: " << ToString(res);
    return false;
  }

  return true;
}

/* static */ bool GpuDriver::SynchronizeContext(GpuContext* context) {
  ScopedActivateContext activation(context);
  CUresult res = cuCtxSynchronize();
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "could not synchronize on CUDA context: " << ToString(res)
               << " :: " << tsl::CurrentStackTrace();
    return false;
  }

  return true;
}

/* static */ tsl::Status GpuDriver::SynchronizeStream(GpuContext* context,
                                                      CUstream stream) {
  ScopedActivateContext activated{context};
  CHECK(stream != nullptr);
  RETURN_IF_CUDA_RES_ERROR(cuStreamSynchronize(stream),
                           "Could not synchronize CUDA stream");
  return ::tsl::OkStatus();
}

/* static */ bool GpuDriver::IsStreamIdle(GpuContext* context,
                                          CUstream stream) {
  ScopedActivateContext activated{context};
  CHECK(stream != nullptr);
  CUresult res = cuStreamQuery(stream);
  if (res == CUDA_SUCCESS) {
    return true;
  }

  if (res != CUDA_ERROR_NOT_READY) {
    LOG(ERROR) << "stream in bad state on status query: " << ToString(res);
  }
  return false;
}

/* static */ tsl::Status GpuDriver::SynchronousMemcpyD2H(GpuContext* context,
                                                         void* host_dst,
                                                         CUdeviceptr gpu_src,
                                                         uint64_t size) {
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(
      cuMemcpyDtoH(host_dst, gpu_src, size),
      absl::StrFormat("failed to synchronous memcpy from device to host "
                      "host dst: %p; GPU src: %p; size: %u=0x%x",
                      host_dst, absl::bit_cast<void*>(gpu_src), size, size));
  VLOG(2) << "successfully sync memcpy'd d2h of " << size << " bytes to "
          << host_dst;
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::SynchronousMemcpyH2D(GpuContext* context,
                                                         CUdeviceptr gpu_dst,
                                                         const void* host_src,
                                                         uint64_t size) {
  ScopedActivateContext activation(context);
  RETURN_IF_CUDA_RES_ERROR(
      cuMemcpyHtoD(gpu_dst, host_src, size),
      absl::StrFormat(
          "failed to synchronous memcpy from host to device: GPU dst: %p;"
          " host src: %p; size: %u=0x%x",
          absl::bit_cast<void*>(gpu_dst), host_src, size, size));
  VLOG(2) << "successfully enqueued sync memcpy h2d of " << size << " bytes";
  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::SynchronousMemcpyD2D(GpuContext* context,
                                                         CUdeviceptr gpu_dst,
                                                         CUdeviceptr gpu_src,
                                                         uint64_t size) {
  ScopedActivateContext activation(context);

  CUresult result;
  // CreatedContexts::GetAnyContext() doesn't works when ptr == 0.
  // This happens when the size is 0.
  if (gpu_dst == 0 || gpu_src == 0) {
    result = cuMemcpyDtoD(gpu_dst, gpu_src, size);
  } else {
    // Any context work here.
    CUcontext dst_context =
        CreatedContexts::GetAnyContext(absl::bit_cast<void*>(gpu_dst));
    CUcontext src_context =
        CreatedContexts::GetAnyContext(absl::bit_cast<void*>(gpu_src));

    if (static_cast<void*>(dst_context) == nullptr) {
      tsl::StatusOr<GpuContext*> tmp_context = GetPointerContext(gpu_dst);
      if (tmp_context.ok()) {
        dst_context = tmp_context.value()->context();
      }
    }

    if (static_cast<void*>(src_context) == nullptr) {
      tsl::StatusOr<GpuContext*> tmp_context = GetPointerContext(gpu_src);
      if (tmp_context.ok()) {
        src_context = tmp_context.value()->context();
      }
    }

    result = cuMemcpyPeer(gpu_dst, dst_context, gpu_src, src_context, size);
  }

  RETURN_IF_CUDA_RES_ERROR(
      result,
      absl::StrFormat(
          "failed to synchronous memcpy from host to device: GPU dst: %p; "
          "GPU src: %p; size: %u=0x%x",
          absl::bit_cast<void*>(gpu_dst), absl::bit_cast<void*>(gpu_src), size,
          size));
  VLOG(2) << "successfully sync memcpy'd d2d of " << size << " bytes";
  return ::tsl::OkStatus();
}

/* static */ bool GpuDriver::AsynchronousMemcpyD2H(GpuContext* context,
                                                   void* host_dst,
                                                   CUdeviceptr gpu_src,
                                                   uint64_t size,
                                                   CUstream stream) {
  ScopedActivateContext activation(context);
  CUresult res = cuMemcpyDtoHAsync(host_dst, gpu_src, size, stream);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << absl::StrFormat(
        "failed to enqueue async memcpy from device to host: %s; host dst: %p; "
        "GPU src: %p; size: %u=0x%x",
        ToString(res), host_dst, absl::bit_cast<void*>(gpu_src), size, size);
    return false;
  }
  VLOG(2) << "successfully enqueued async memcpy d2h of " << size
          << " bytes from " << absl::bit_cast<void*>(gpu_src) << " to "
          << host_dst << " on stream " << stream;
  return true;
}

/* static */ bool GpuDriver::AsynchronousMemcpyH2D(GpuContext* context,
                                                   CUdeviceptr gpu_dst,
                                                   const void* host_src,
                                                   uint64_t size,
                                                   CUstream stream) {
  ScopedActivateContext activation(context);
  CUresult res = cuMemcpyHtoDAsync(gpu_dst, host_src, size, stream);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << absl::StrFormat(
        "failed to enqueue async memcpy from host to device: %s; GPU dst: %p; "
        "host src: %p; size: %u=0x%x",
        ToString(res), absl::bit_cast<void*>(gpu_dst), host_src, size, size);
    return false;
  }
  VLOG(2) << "successfully enqueued async memcpy h2d of " << size << " bytes"
          << " from " << host_src << " to " << absl::bit_cast<void*>(gpu_dst)
          << " on stream " << stream;
  return true;
}

/* static */ bool GpuDriver::AsynchronousMemcpyD2D(GpuContext* context,
                                                   CUdeviceptr gpu_dst,
                                                   CUdeviceptr gpu_src,
                                                   uint64_t size,
                                                   CUstream stream) {
  ScopedActivateContext activation(context);
  CUresult result;

  // Check if the stream is doing graph capture.
  cudaStreamCaptureStatus stream_capture_status;
  cudaError_t err =
      cudaStreamGetCaptureInfo(stream, &stream_capture_status, /*pId=*/nullptr);
  if (err != cudaSuccess) {
    LOG(ERROR) << "Failed to get stream capture info: "
               << cudaGetErrorString(err);
    return false;
  }

  // In graph capture mode we never have operations that access peer memory, so
  // we can always make a call to cuMemcpyDtoDAsync.
  bool is_capturing = stream_capture_status == cudaStreamCaptureStatusActive;

  if ((gpu_dst == 0 || gpu_src == 0) || is_capturing) {
    // CreatedContexts::GetAnyContext() doesn't works when ptr == 0.
    // This happens when the size is 0.
    result = cuMemcpyDtoDAsync(gpu_dst, gpu_src, size, stream);
  } else {
    // Any context work here.
    CUcontext dst_context =
        CreatedContexts::GetAnyContext(absl::bit_cast<void*>(gpu_dst));
    CUcontext src_context =
        CreatedContexts::GetAnyContext(absl::bit_cast<void*>(gpu_src));

    if (static_cast<void*>(dst_context) == nullptr) {
      tsl::StatusOr<GpuContext*> tmp_context = GetPointerContext(gpu_dst);
      if (tmp_context.ok()) {
        dst_context = tmp_context.value()->context();
      }
    }

    if (static_cast<void*>(src_context) == nullptr) {
      tsl::StatusOr<GpuContext*> tmp_context = GetPointerContext(gpu_src);
      if (tmp_context.ok()) {
        src_context = tmp_context.value()->context();
      }
    }

    if (dst_context == src_context) {
      // Since the CUDA context is the same, the src and dst are within the same
      // GPU. So we can use cuMemcpyDtoD.
      result = cuMemcpyDtoDAsync(gpu_dst, gpu_src, size, stream);
    } else {
      result = cuMemcpyPeerAsync(gpu_dst, dst_context, gpu_src, src_context,
                                 size, stream);
    }
  }
  if (result != CUDA_SUCCESS) {
    LOG(ERROR) << absl::StrFormat(
        "failed to enqueue async memcpy from device to device: %s"
        "; GPU dst: %p on %s %s"
        "; GPU src: %p on %s %s"
        "; can access? %s; size: %u=0x%x",
        ToString(result), absl::bit_cast<void*>(gpu_dst),
        CUDAPointerToMemorySpaceString(gpu_dst),
        CUDAPointerToDeviceString(gpu_dst), absl::bit_cast<void*>(gpu_src),
        CUDAPointerToMemorySpaceString(gpu_src),
        CUDAPointerToDeviceString(gpu_src),
        CUDAPointersToCanAccessString(gpu_src, gpu_dst), size, size);

    return false;
  }
  VLOG(2) << "successfully enqueued async memcpy d2d of " << size << " bytes"
          << " from " << absl::bit_cast<void*>(gpu_src) << " to "
          << absl::bit_cast<void*>(gpu_dst) << " on stream " << stream;
  return true;
}

/* static */ tsl::Status GpuDriver::InitEvent(GpuContext* context,
                                              CUevent* result,
                                              EventFlags flags) {
  int cuflags;
  switch (flags) {
    case EventFlags::kDefault:
      cuflags = CU_EVENT_DEFAULT;
      break;
    case EventFlags::kDisableTiming:
      cuflags = CU_EVENT_DISABLE_TIMING;
      break;
    default:
      LOG(FATAL) << "impossible event flags: " << int(flags);
  }

  ScopedActivateContext activated{context};
  CUresult res = cuEventCreate(result, cuflags);

  if (res == CUDA_SUCCESS) {
    return ::tsl::OkStatus();
  } else if (res == CUDA_ERROR_OUT_OF_MEMORY) {
    return tsl::Status(absl::StatusCode::kResourceExhausted,
                       "could not create CUDA event: out of device memory");
  } else {
    return tsl::Status(
        absl::StatusCode::kFailedPrecondition,
        absl::StrCat("could not create CUDA event: ", ToString(res)));
  }
}

/* static */ int GpuDriver::GetDeviceCount() {
  int device_count = 0;
  CUresult res = cuDeviceGetCount(&device_count);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "could not retrieve CUDA device count: " << ToString(res);
    return 0;
  }

  if (FLAGS_gpuexec_cuda_device_0_only && device_count > 1) {
    device_count = 1;
  }
  return device_count;
}

/* static */ tsl::StatusOr<GpuContext*> GpuDriver::GetPointerContext(
    CUdeviceptr pointer) {
  GpuContext* context = nullptr;
  CUresult result =
      cuPointerGetAttribute(&context, CU_POINTER_ATTRIBUTE_CONTEXT, pointer);
  if (result == CUDA_SUCCESS) {
    // For cudaMallocAsync, the context returned is null.  For now
    // return not-available. But how to manage that correctly
    // everywhere in TF?  Currently this is only used during error
    // handling.  So all is working fine, but TF have a different
    // error then the original one.
    if (context == nullptr) {
      return tsl::Status(
          absl::StatusCode::kUnavailable,
          "Empty context returned while querying context for device pointer");
    }
    return context;
  }

  return tsl::Status(
      absl::StatusCode::kInternal,
      absl::StrCat("failed to query context for device pointer: ",
                   ToString(result)));
}

/* static */ tsl::StatusOr<MemorySpace> GpuDriver::GetPointerMemorySpace(
    CUdeviceptr pointer) {
  unsigned int value;
  CUresult result =
      cuPointerGetAttribute(&value, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, pointer);
  if (result == CUDA_SUCCESS) {
    switch (value) {
      case CU_MEMORYTYPE_DEVICE:
        return MemorySpace::kDevice;
      case CU_MEMORYTYPE_HOST:
        return MemorySpace::kHost;
      default:
        return tsl::Status(
            absl::StatusCode::kInternal,
            absl::StrCat("unknown memory space provided by CUDA API: ", value));
    }
  }

  return tsl::Status(
      absl::StatusCode::kInternal,
      absl::StrCat("failed to query device pointer for memory space: ",
                   ToString(result)));
}

/* static */ tsl::Status GpuDriver::GetPointerAddressRange(CUdeviceptr dptr,
                                                           CUdeviceptr* base,
                                                           size_t* size) {
  CUresult result = cuMemGetAddressRange(base, size, dptr);
  if (result == CUDA_SUCCESS) {
    return ::tsl::OkStatus();
  } else if (result == CUDA_ERROR_NOT_FOUND) {
    // We differentiate between "this pointer is unknown" (return here) and
    // "there was an internal error while performing this operation" (return
    // below).
    return tsl::Status(
        absl::StatusCode::kNotFound,
        absl::StrFormat("not a device pointer %p; %s",
                        reinterpret_cast<void*>(dptr), ToString(result)));
  }

  return tsl::Status(
      absl::StatusCode::kInternal,
      absl::StrFormat("failed to get pointer into for device pointer %p; %s",
                      reinterpret_cast<void*>(dptr), ToString(result)));
}

/* static */ tsl::StatusOr<CUdevice> GpuDriver::GetPointerDevice(
    CUdeviceptr pointer) {
  auto result = GetPointerContext(pointer);
  if (!result.ok()) {
    return result.status();
  }

  return DeviceFromContext(result.value());
}

/* static */ tsl::Status GpuDriver::GetComputeCapability(int* cc_major,
                                                         int* cc_minor,
                                                         CUdevice device) {
  *cc_major = 0;
  *cc_minor = 0;

  CUresult res = cuDeviceGetAttribute(
      cc_major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device);
  if (res != CUDA_SUCCESS) {
    return tsl::Status(
        absl::StatusCode::kInternal,
        absl::StrFormat(
            "failed to get compute capability major for device: %s; %d",
            ToString(res), device));
  }

  res = cuDeviceGetAttribute(
      cc_minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device);
  if (res != CUDA_SUCCESS) {
    return tsl::Status(
        absl::StatusCode::kInternal,
        absl::StrFormat(
            "failed to get compute capability minor for device: %s; %d",
            ToString(res), device));
  }

  return ::tsl::OkStatus();
}

/* static */ tsl::Status GpuDriver::GetGpuISAVersion(int* version,
                                                     CUdevice device) {
  return tsl::Status{
      absl::StatusCode::kInternal,
      "Feature not supported on CUDA platform (GetGpuISAVersion)"};
}

/* static */ tsl::Status GpuDriver::GetGpuGCNArchName(CUdevice, std::string*) {
  return tsl::Status{
      absl::StatusCode::kInternal,
      "Feature not supported on CUDA platform (GetGpuGCNArchName)"};
}

// Helper function that turns the integer output of cuDeviceGetAttribute to type
// T and wraps it in a StatusOr.
template <typename T>
static tsl::StatusOr<T> GetSimpleAttribute(CUdevice device,
                                           CUdevice_attribute attribute) {
  int value = -1;
  RETURN_IF_CUDA_RES_ERROR(cuDeviceGetAttribute(&value, attribute, device),
                           "Could not retrieve CUDA device attribute (",
                           attribute);
  T converted = value;
  return converted;
}

/* static */ tsl::StatusOr<int> GpuDriver::GetMultiprocessorCount(
    CUdevice device) {
  return GetSimpleAttribute<int>(device,
                                 CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT);
}

/* static */ tsl::StatusOr<int64_t> GpuDriver::GetMaxSharedMemoryPerCore(
    CUdevice device) {
  return GetSimpleAttribute<int64_t>(
      device, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_MULTIPROCESSOR);
}

/* static */ tsl::StatusOr<int64_t> GpuDriver::GetMaxSharedMemoryPerBlock(
    CUdevice device) {
  return GetSimpleAttribute<int64_t>(
      device, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK);
}

tsl::StatusOr<int64_t> GpuDriver::GetMaxSharedMemoryPerBlockOptin(
    CUdevice device) {
  return GetSimpleAttribute<int64_t>(
      device, CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK_OPTIN);
}

/* static */ tsl::StatusOr<int64_t> GpuDriver::GetMaxThreadsPerMultiprocessor(
    CUdevice device) {
  return GetSimpleAttribute<int64_t>(
      device, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_MULTIPROCESSOR);
}

/* static */ tsl::StatusOr<int64_t> GpuDriver::GetMaxThreadsPerBlock(
    CUdevice device) {
  return GetSimpleAttribute<int64_t>(device,
                                     CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK);
}

/* static */ tsl::StatusOr<int64_t> GpuDriver::GetMaxRegistersPerBlock(
    CUdevice device) {
  return GetSimpleAttribute<int64_t>(
      device, CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK);
}

/* static */ tsl::StatusOr<int64_t> GpuDriver::GetThreadsPerWarp(
    CUdevice device) {
  return GetSimpleAttribute<int64_t>(device, CU_DEVICE_ATTRIBUTE_WARP_SIZE);
}

/* static */ tsl::Status GpuDriver::GetGridLimits(int* x, int* y, int* z,
                                                  CUdevice device) {
  int value;
  RETURN_IF_CUDA_RES_ERROR(
      cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X, device),
      "Could not get device attribute");
  *x = value;

  RETURN_IF_CUDA_RES_ERROR(
      cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y, device),
      "Could not get device attribute");
  *y = value;

  RETURN_IF_CUDA_RES_ERROR(
      cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z, device),
      "Could not get device attribute");
  *z = value;
  return tsl::OkStatus();
}

/* static */ bool GpuDriver::GetDriverVersion(int* driver_version) {
  CUresult res = cuDriverGetVersion(driver_version);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to query driver version: " << ToString(res);
    return false;
  }

  return true;
}

/* static */ bool GpuDriver::GetDeviceProperties(CUdevprop* device_properties,
                                                 int device_ordinal) {
  CUresult res = cuDeviceGetProperties(device_properties, device_ordinal);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to query device properties: " << ToString(res);
    return false;
  }

  return true;
}

/* static */ tsl::StatusOr<int> GpuDriver::GetDeviceAttribute(
    CUdevice_attribute attribute, CUdevice device) {
  int val;
  CUresult res = cuDeviceGetAttribute(&val, attribute, device);
  if (res != CUDA_SUCCESS) {
    return tsl::Status(
        absl::StatusCode::kInternal,
        absl::StrFormat("failed to get device attribute %d for device %d: %s",
                        attribute, device, ToString(res)));
  }
  return val;
}

/* static */ bool GpuDriver::IsEccEnabled(CUdevice device, bool* result) {
  int value = -1;
  CUresult res =
      cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_ECC_ENABLED, device);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to query ECC status: " << ToString(res);
    return false;
  }

  *result = value;
  return true;
}

/* static */ bool GpuDriver::GetDeviceMemoryInfo(GpuContext* context,
                                                 int64_t* free_out,
                                                 int64_t* total_out) {
  ScopedActivateContext activation(context);
  size_t free = 0;
  size_t total = 0;
  CUresult res = cuMemGetInfo(&free, &total);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to query device memory info: " << ToString(res);
    return false;
  }

  *free_out = free;
  *total_out = total;
  return true;
}

/* static */ bool GpuDriver::GetDeviceTotalMemory(CUdevice device,
                                                  uint64_t* result) {
  size_t value = -1;
  CUresult res = cuDeviceTotalMem(&value, device);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to query total available memory: " << ToString(res);
    return false;
  }

  *result = value;
  return true;
}

/* static */ std::string GpuDriver::GetPCIBusID(CUdevice device) {
  std::string pci_bus_id;
  static const int kBufferSize = 64;
  absl::InlinedVector<char, 4> chars(kBufferSize);
  chars[kBufferSize - 1] = '\0';
  CUresult res = cuDeviceGetPCIBusId(chars.begin(), kBufferSize - 1, device);
  if (res != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to query PCI bus id for device: " << ToString(res);
    return pci_bus_id;
  }
  pci_bus_id = chars.begin();
  return pci_bus_id;
}

/* static */ bool GpuDriver::CanEnablePeerAccess(GpuContext* from,
                                                 GpuContext* to) {
  if (from == to) {
    return true;  // A context can always access its own memory.
  }

  auto from_device = DeviceFromContext(from);
  if (!from_device.ok()) {
    LOG(ERROR) << "failed to resolve 'from' peer access context to a device: "
               << from_device.status();
    return false;
  }
  auto to_device = DeviceFromContext(to);
  if (!to_device.ok()) {
    LOG(ERROR) << "failed to resolve 'to' peer access context to a device: "
               << to_device.status();
    return false;
  }
  return CanEnablePeerAccess(from_device.value(), to_device.value());
}

/* static */ bool GpuDriver::CanEnablePeerAccess(GpuDeviceHandle from,
                                                 GpuDeviceHandle to) {
  int can_access_peer = -1;
  CUresult result = cuDeviceCanAccessPeer(&can_access_peer, from, to);
  if (result != CUDA_SUCCESS) {
    LOG(ERROR) << "failed to detect peer access capability: "
               << ToString(result);
    return false;
  }
  return can_access_peer;
}

/* static */ tsl::Status GpuDriver::EnablePeerAccess(GpuContext* from,
                                                     GpuContext* to) {
  if (from == to) {
    return ::tsl::OkStatus();  // A context can always access its own
                               // memory.
  }

  ScopedActivateContext activated{from};
  CUresult result = cuCtxEnablePeerAccess(to->context(), 0 /* = flags */);
  if (result != CUDA_SUCCESS &&
      result != CUDA_ERROR_PEER_ACCESS_ALREADY_ENABLED) {
    return tsl::Status(
        absl::StatusCode::kInternal,
        absl::StrFormat("failed to enable peer access from %p to %p: %s", from,
                        to, ToString(result)));
  }

  return ::tsl::OkStatus();
}

/* static */ tsl::StatusOr<int> GpuDriver::GetMaxOccupiedBlocksPerCore(
    GpuContext* context, CUfunction kernel, int threads_per_block,
    size_t dynamic_shared_memory_bytes) {
  ScopedActivateContext activation(context);

  int max_blocks;
  RETURN_IF_CUDA_RES_ERROR(
      cuOccupancyMaxActiveBlocksPerMultiprocessor(
          &max_blocks, kernel, threads_per_block, dynamic_shared_memory_bytes),
      absl::StrFormat("Failed to calculate occupancy of kernel %p", kernel));
  return max_blocks;
}

}  // namespace gpu

namespace cuda {

CUcontext CurrentContextOrDie() {
  CUcontext current = nullptr;
  FAIL_IF_CUDA_RES_ERROR(cuCtxGetCurrent(&current),
                         "Failed to query current context");
  return current;
}

}  // namespace cuda
}  // namespace stream_executor
