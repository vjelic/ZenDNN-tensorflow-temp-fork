# Description:
#  Tools for building the TensorFlow pip package.

load("@local_config_cuda//cuda:build_defs.bzl", "if_cuda")
load("@local_config_syslibs//:build_defs.bzl", "if_not_system_lib")
load("//tensorflow:tensorflow.bzl", "filegroup_as_file", "transitive_hdrs")
load("//tensorflow/core/platform:build_config_root.bzl", "tf_additional_license_deps")
load("//third_party/mkl:build_defs.bzl", "if_enable_mkl", "if_mkl", "if_mkl_ml")

package(default_visibility = ["//visibility:private"])

# This returns a list of headers of all public header libraries (e.g.,
# framework, lib), and all of the transitive dependencies of those
# public headers.  Not all of the headers returned by the filegroup
# are public (e.g., internal headers that are included by public
# headers), but the internal headers need to be packaged in the
# pip_package for the public headers to be properly included.
#
# Public headers are therefore defined by those that are both:
#
# 1) "publicly visible" as defined by bazel
# 2) Have documentation.
#
# This matches the policy of "public" for our python API.
transitive_hdrs(
    name = "included_headers",
    deps = [
        "//tensorflow/compiler/tf2xla:xla_compiled_cpu_function",
        "//tensorflow/compiler/mlir:mlir_graph_optimization_pass",
        "//tensorflow/core:core_cpu",
        "//tensorflow/core:framework",
        "//tensorflow/core:lib",
        "//tensorflow/core:protos_all_cc",
        "//tensorflow/core/platform:stream_executor",
        "//tensorflow/cc/saved_model:loader",
        "//tensorflow/cc/saved_model:reader",
        "//tensorflow/cc/saved_model:bundle_v2",
        "//tensorflow/c/experimental/filesystem:filesystem_interface",
        "//tensorflow/c/experimental/next_pluggable_device:c_api_hdrs",
        "//tensorflow/c/experimental/pluggable_profiler:pluggable_profiler_hdrs",
        "//tensorflow/c/experimental/stream_executor:stream_executor_hdrs",
        "//tensorflow/c/experimental/grappler:grappler_hdrs",
        "//tensorflow/c:kernels_hdrs",
        "//tensorflow/c:ops_hdrs",
        "@local_xla//xla/pjrt/c:pjrt_c_api_hdrs",
        "//tensorflow/core/common_runtime/next_pluggable_device/c:plugin_c_api_hdrs",
        # TODO(rostam): Revisit these after cc_shared_library everywhere
        "//tensorflow/cc:client_session",
        "//tensorflow/cc:cc_ops",
        # WARNING: None of the C/C++ code under python/ has any API guarantees, and TF team
        # reserves the right to change APIs and other header-level interfaces.  If your custom
        # op uses these headers, it may break when users upgrade their version of tensorflow.
        # NOTE(ebrevdo): None of the python symbols are exported by libtensorflow_framework.so.
        # Code that relies on these headers should dynamically link to
        # _pywrap_tensorflow_internal.so as well.
        "//tensorflow/python/grappler:model_analyzer_lib",
        "//tensorflow/python/lib/core:py_exception_registry",
        "//tensorflow/python/lib/core:pybind11_proto",
        "//tensorflow/python/lib/core:pybind11_absl",
        "//tensorflow/python/lib/core:pybind11_lib",
        "//tensorflow/python/lib/core:pybind11_status",
        "//tensorflow/python/lib/core:py_func_lib",
        "//tensorflow/python/lib/core:py_seq_tensor",
        "//tensorflow/python/lib/core:py_util",
        "//tensorflow/python/util:cpp_python_util",
        "//tensorflow/python/util:kernel_registry",
        "//tensorflow/python/framework:python_op_gen",
        "//tensorflow/python/client:tf_session_helper",
        "//third_party/eigen3",
        "@local_xla//xla/stream_executor:stream_executor_headers",
    ] + if_cuda([
        "@local_config_cuda//cuda:cuda_headers",
    ]),
)

py_binary(
    name = "simple_console",
    srcs = ["simple_console.py"],
    srcs_version = "PY3",
    deps = ["//tensorflow:tensorflow_py"],
)

filegroup_as_file(
    name = "xla_compiled_cpu_runtime_srcs.txt",
    dep = "//tensorflow/compiler/tf2xla:xla_compiled_cpu_runtime_srcs",
)

filegroup(
    name = "xla_cmake",
    srcs = ["xla_build/CMakeLists.txt"],
)

# Add dynamic kernel dso files here.
DYNAMIC_LOADED_KERNELS = [
    "//tensorflow/core/kernels:libtfkernel_sobol_op.so",
]

COMMON_PIP_DEPS = [
    ":licenses",
    "MANIFEST.in",
    "THIRD_PARTY_NOTICES.txt",
    "README",
    "setup.py",
    ":included_headers",
    ":xla_compiled_cpu_runtime_srcs.txt_file",
    ":xla_cmake",
    "//tensorflow/compiler/tf2xla:xla_compiled_cpu_runtime_hdrs",
    "//tensorflow/compiler/tf2xla:xla_compiled_cpu_runtime_srcs",
    "//tensorflow/compiler/mlir/stablehlo:stablehlo",
    "//tensorflow/compiler/mlir/tensorflow:gen_mlir_passthrough_op_py",
    "//tensorflow/core/function/trace_type:serialization_test_proto_py",
    "//tensorflow/core/function/trace_type:serialization",
    "//tensorflow/core:protos_all_proto_srcs",
    "//tensorflow/dtensor/python/tests:multi_client_test_util",
    "//tensorflow/dtensor/python/tests:test_util",
    "//tensorflow/lite/python:tflite_convert",
    "//tensorflow/lite/toco/python:toco_from_protos",
    "//tensorflow/lite/tools:visualize",
    "//tensorflow/python/autograph/core:test_lib",
    "//tensorflow/python/autograph/impl/testing:pybind_for_testing",
    "//tensorflow/python/autograph/pyct/testing:basic_definitions",
    "//tensorflow/python/autograph/pyct/testing:decorators",
    "//tensorflow/python/autograph/pyct/common_transformers:common_transformers",
    "//tensorflow/python/autograph/pyct/static_analysis:type_inference",
    "//tensorflow/python/autograph/utils:context_managers",
    "//tensorflow/python/autograph/utils:tensor_list",
    "//tensorflow/python/compiler:compiler",
    "//tensorflow/python/ops:cond_v2",
    "//tensorflow/python:distributed_framework_test_lib",
    "//tensorflow/python/distribute:distribute_test_lib_pip",
    "//tensorflow/python/training/experimental:loss_scale",
    "//tensorflow/python/training/experimental:loss_scale_optimizer",
    "//tensorflow/python/framework:meta_graph_testdata",
    "//tensorflow/python/data/benchmarks:benchmark_base",
    "//tensorflow/python/data/experimental/kernel_tests/service:multi_process_cluster",
    "//tensorflow/python/data/experimental/kernel_tests/service:test_base",
    "//tensorflow/python/data/experimental/ops:testing",
    "//tensorflow/python/data/experimental/service:server_lib",
    "//tensorflow/python/data/kernel_tests:checkpoint_test_base",
    "//tensorflow/python/data/kernel_tests:test_base",
    "//tensorflow/python/data/kernel_tests:tf_record_test_base",
    "//tensorflow/python/ops/ragged:ragged_tensor_test_ops",
    "//tensorflow/python/debug:debug_pip",
    "//tensorflow/python/distribute:combinations",
    "//tensorflow/python/distribute/coordinator:fault_tolerance_test_base",
    "//tensorflow/python/distribute/failure_handling:check_preemption_py",
    "//tensorflow/python/distribute/failure_handling:failure_handling_lib",
    "//tensorflow/python/distribute/failure_handling:failure_handling_util",
    "//tensorflow/python/distribute:multi_process_runner",
    "//tensorflow/python/eager:eager_pip",
    "//tensorflow/python/keras:combinations",
    "//tensorflow/python/keras/mixed_precision:test_util",
    "//tensorflow/python/keras/utils:dataset_creator",
    "//tensorflow/python/kernel_tests/linalg/testdata:self_adjoint_eig_op_test_files",
    "//tensorflow/python/kernel_tests/nn_ops:bias_op_base",
    "//tensorflow/python/kernel_tests/nn_ops:cudnn_deterministic_base",
    "//tensorflow/python/kernel_tests/nn_ops:depthwise_conv_op_base",
    "//tensorflow/python/kernel_tests/nn_ops:xent_op_test_base",
    "//tensorflow/python/kernel_tests/random:util",
    "//tensorflow/python/kernel_tests/signal:test_util",
    "//tensorflow/python/kernel_tests/sparse_ops:sparse_xent_op_test_base",
    "//tensorflow/python/lib:__init__",
    "//tensorflow/python/ops/parallel_for:test_util",
    "//tensorflow/python/ops/structured:structured_tensor_dynamic",
    "//tensorflow/python/platform:resource_loader",
    "//tensorflow/python/profiler:trace",
    "//tensorflow/python/saved_model:saved_model",
    "//tensorflow/python/summary:__init__",
    "//tensorflow/python/summary:plugin_asset",
    "//tensorflow/python/summary:summary_iterator",
    "//tensorflow/python/summary:summary_py",
    "//tensorflow/python/tools:tools_pip",
    "//tensorflow/python/tools/api/generator:create_python_api",
    "//tensorflow/python/tools/api/generator2/extractor:extractor",
    "//tensorflow/python/tools/api/generator2/generator:generator",
    "//tensorflow/python/tpu",
    "//tensorflow/python/util:deprecated_module",
    "//tensorflow/python/util:deprecated_module_new",
    "//tensorflow/python/util:example_parser_configuration",
    "//tensorflow/python/util:function_utils",
    "//tensorflow/python/util:keyword_args",
    "//tensorflow/python/util:lock_util",
    "//tensorflow/python/util:module_wrapper",
    "//tensorflow/python/util:serialization",
    "//tensorflow/python/ops:image_grad_test_base",
    "//tensorflow/python/framework:memory_checker",
    "//tensorflow/python/framework:test_ops",
    "//tensorflow/python/ops:while_v2",
    "//tensorflow/tools/common:public_api",
    "//tensorflow/tools/common:test_module1",
    "//tensorflow/tools/common:traverse",
    "//tensorflow/tools/docs:tf_doctest_lib",
    "@local_tsl//tsl/profiler/protobuf:trace_events_proto_py",
    "//tensorflow/python/distribute:parameter_server_strategy_v2",
    "//tensorflow/python/distribute/coordinator:cluster_coordinator",
    "//tensorflow/python/distribute/coordinator:remote_eager_lib",
    "//tensorflow/python/distribute/coordinator:metric_utils",
    "//tensorflow/python/distribute/experimental/rpc:rpc_ops",
    "//tensorflow/python/util:pywrap_xla_ops",
    "//tensorflow:tensorflow_py",
]

filegroup(
    name = "licenses",
    data = [
        "//:LICENSE",
        "//third_party/icu/data:LICENSE",
        "@com_google_absl//:LICENSE",
        "@com_google_protobuf//:LICENSE",
        "@curl//:COPYING",
        "@ducc//:LICENSE",
        "@flatbuffers//:LICENSE",
        "@gemmlowp//:LICENSE",
        "@libjpeg_turbo//:LICENSE.md",
        "@llvm-project//llvm:LICENSE.TXT",
        "@llvm-project//mlir:LICENSE.TXT",
        "@local_config_tensorrt//:LICENSE",
        "@ml_dtypes//:LICENSE",
        "@org_brotli//:LICENSE",
        "@pasta//:LICENSE",
        "@png//:LICENSE",
        "@ruy//:LICENSE",
        "@six_archive//:LICENSE",
        "@snappy//:COPYING",
        "@stablehlo//:LICENSE",
        "@tf_runtime//:LICENSE",
    ] + select({
        "//tensorflow:android": [],
        "//tensorflow:ios": [],
        "//tensorflow:linux_s390x": [],
        "//tensorflow:windows": [],
        "//conditions:default": [
            "@com_github_googlecloudplatform_google_cloud_cpp//:LICENSE",
        ],
    }) + select({
        "@local_tsl//tsl:with_numa_support": [
            "@hwloc//:COPYING",
        ],
        "//conditions:default": [],
    }) + if_cuda([
        "@cub_archive//:LICENSE.TXT",
        "@local_config_nccl//:LICENSE",
    ]) + if_mkl([
        "//third_party/mkl_dnn:LICENSE",
    ]) + if_enable_mkl(["//third_party/mkl:LICENSE"]) + if_not_system_lib(
        "absl_py",
        [
            "@absl_py//:LICENSE",
        ],
    ) + tf_additional_license_deps(),
)

sh_binary(
    name = "build_pip_package",
    srcs = ["build_pip_package.sh"],
    data = COMMON_PIP_DEPS +
           select({
               "//tensorflow:windows": [
                   "//tensorflow/python:pywrap_tensorflow_import_lib_file",
               ],
               "//conditions:default": [],
           }) +
           select({
               "//tensorflow:dynamic_loaded_kernels": DYNAMIC_LOADED_KERNELS,
               "//conditions:default": [],
           }) + if_mkl_ml(["//third_party/mkl:intel_binary_blob"]) + [
        "//tensorflow:tf_python_api_gen_v2",
    ],
    visibility = ["//visibility:public"],
)
