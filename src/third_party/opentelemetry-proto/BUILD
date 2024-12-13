load("//bazel:mongo_src_rules.bzl", "mongo_cc_binary", "mongo_cc_library", "mongo_cc_proto_library", "mongo_proto_library")
load("//src/third_party/opentelemetry-proto:otel_rules.bzl", "OTEL_TARGET_COMPATIBLE_WITH", "mongo_cc_proto_lib")

package(default_visibility = ["//visibility:public"])

mongo_proto_library(
    name = "common_proto",
    srcs = [
        "opentelemetry/proto/common/v1/common.proto",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
)

mongo_cc_proto_lib(
    name = "common_proto_cc",
    deps = [":common_proto"],
)

mongo_proto_library(
    name = "resource_proto",
    srcs = [
        "opentelemetry/proto/resource/v1/resource.proto",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        ":common_proto",
    ],
)

mongo_cc_proto_lib(
    name = "resource_proto_cc",
    deps = [":resource_proto"],
)

mongo_proto_library(
    name = "trace_proto",
    srcs = [
        "opentelemetry/proto/trace/v1/trace.proto",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        ":common_proto",
        ":resource_proto",
    ],
)

mongo_cc_proto_lib(
    name = "trace_proto_cc",
    deps = [":trace_proto"],
)

mongo_proto_library(
    name = "trace_service_proto",
    srcs = [
        "opentelemetry/proto/collector/trace/v1/trace_service.proto",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        ":trace_proto",
    ],
)

mongo_cc_proto_lib(
    name = "trace_service_proto_cc",
    deps = [":trace_service_proto"],
)
