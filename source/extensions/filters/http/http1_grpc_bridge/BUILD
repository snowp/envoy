licenses(["notice"])  # Apache 2

# L7 HTTP filter that bridges unary gRPC to HTTP/1.1 

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
    "envoy_select_google_grpc",
)

envoy_package()

envoy_cc_library(
    name = "grpc_bridge_filter_lib",
    srcs = ["http1_bridge_filter.cc"],
    hdrs = ["http1_bridge_filter.h"],
    deps = [
        "//include/envoy/http:codes_interface",
        "//include/envoy/http:filter_interface",
        "//include/envoy/upstream:cluster_manager_interface",
        "//source/common/common:enum_to_int",
        "//source/common/buffer:buffer_lib",
        "//source/common/common:utility_lib",
        "//source/common/grpc:common_lib",
        "//source/common/http:headers_lib",
        "//source/common/http/http1:codec_lib",
    ],
)

envoy_cc_library(
    name = "config",
    srcs = ["config.cc"],
    hdrs = ["config.h"],
    deps = [
        "//include/envoy/registry",
        "//include/envoy/server:filter_config_interface",
        "//source/extensions/filters/http:well_known_names",
        "//source/extensions/filters/http/common:empty_http_filter_config_lib",
        "//source/extensions/filters/http/http1_grpc_bridge:grpc_bridge_filter_lib",
    ],
)
