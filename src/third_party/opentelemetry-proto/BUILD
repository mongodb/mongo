load(
    "//bazel:mongo_src_rules.bzl",
    "mongo_cc_binary",
    "mongo_cc_grpc_library",
    "mongo_cc_library",
    "mongo_cc_proto_library",
    "mongo_proto_library",
)
load(
    "//src/third_party/opentelemetry-proto:otel_rules.bzl",
    "OTEL_COPTS",
    "OTEL_TARGET_COMPATIBLE_WITH",
    "mongo_cc_proto_lib",
)

package(default_visibility = ["//visibility:public"])

GRPC_TARGET_COMPATIBLE_WITH = select({
    "//bazel/config:build_grpc_enabled": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

mongo_proto_library(
    name = "common_proto",
    srcs = [
        "opentelemetry/proto/common/v1/common.proto",
    ],
    strip_import_prefix = "/src/third_party/opentelemetry-proto/",
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
    strip_import_prefix = "/src/third_party/opentelemetry-proto/",
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
    name = "logs_proto",
    srcs = [
        "opentelemetry/proto/logs/v1/logs.proto",
    ],
    strip_import_prefix = "/src/third_party/opentelemetry-proto/",
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        ":common_proto",
        ":resource_proto",
    ],
)

mongo_cc_proto_lib(
    name = "logs_proto_cc",
    deps = [
        ":logs_proto",
    ],
)

mongo_proto_library(
    name = "trace_proto",
    srcs = [
        "opentelemetry/proto/trace/v1/trace.proto",
    ],
    strip_import_prefix = "/src/third_party/opentelemetry-proto/",
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

# Note: The naming convention for a proto called foo.proto is:
#   proto_library      -> "foo_proto"
#   grpc_proto_library -> "foo"
#   cc_proto_library   -> "foo_cc_proto"
#   cc_grpc_library    -> "foo_cc_grpc"
#
# This naming convention does not apply to proto targets above this
# comment due to mongo_cc_proto_lib usage (defined in otel_rules.bzl).

mongo_proto_library(
    name = "logs_service_proto",
    srcs = [
        "opentelemetry/proto/collector/logs/v1/logs_service.proto",
    ],
    strip_import_prefix = "/src/third_party/opentelemetry-proto/",
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        ":logs_proto",
    ],
)

mongo_cc_proto_library(
    name = "logs_service_cc_proto",
    deps = [":logs_service_proto"],
)

mongo_cc_grpc_library(
    name = "logs_service_cc_grpc",
    srcs = [":logs_service_proto"],
    cc_proto = ":logs_service_cc_proto",
    generate_mocks = True,
    no_undefined_ref_DO_NOT_USE = False,
    target_compatible_with = GRPC_TARGET_COMPATIBLE_WITH,
)

mongo_cc_library(
    name = "logs_service",
    srcs = [],
    hdrs = [],
    copts = OTEL_COPTS,
    target_compatible_with = GRPC_TARGET_COMPATIBLE_WITH,
    deps = [
        ":common_proto_cc",
        ":logs_proto_cc",
        ":logs_service_cc_grpc",
        ":resource_proto_cc",
        "//src/third_party/grpc:grpc++_reflection",
        "//src/third_party/protobuf",
    ],
)

mongo_cc_proto_library(
    name = "metrics_service_cc_proto",
    deps = [":metrics_service_proto"],
)

mongo_cc_grpc_library(
    name = "metrics_service_cc_grpc",
    srcs = [":metrics_service_proto"],
    cc_proto = ":metrics_service_cc_proto",
    generate_mocks = True,
    no_undefined_ref_DO_NOT_USE = False,
    target_compatible_with = GRPC_TARGET_COMPATIBLE_WITH,
)

mongo_cc_library(
    name = "metrics_service",
    srcs = [],
    hdrs = [],
    copts = OTEL_COPTS,
    target_compatible_with = GRPC_TARGET_COMPATIBLE_WITH,
    deps = [
        ":common_proto_cc",
        ":metrics_proto_cc",
        ":metrics_service_cc_grpc",
        ":resource_proto_cc",
        "//src/third_party/grpc:grpc++_reflection",
        "//src/third_party/protobuf",
    ],
)

mongo_proto_library(
    name = "trace_service_proto",
    srcs = [
        "opentelemetry/proto/collector/trace/v1/trace_service.proto",
    ],
    strip_import_prefix = "/src/third_party/opentelemetry-proto/",
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        ":trace_proto",
    ],
)

# For backwards compatibility with otlp/file and otlp/http exporters
# that include trace_service.pb.h but not trace_service.grpc.pb.h (in
# the otlp/grpc exporter).
mongo_cc_proto_lib(
    name = "trace_service_proto_cc_base",
    deps = [":trace_service_proto"],
)

mongo_cc_library(
    name = "trace_service_proto_cc",
    deps = select({
        "//bazel/config:build_grpc_enabled": [":trace_service"],
        "//conditions:default": [":trace_service_proto_cc_base"],
    }),
)

mongo_cc_proto_library(
    name = "trace_service_cc_proto",
    deps = [":trace_service_proto"],
)

mongo_cc_grpc_library(
    name = "trace_service_cc_grpc",
    srcs = [":trace_service_proto"],
    cc_proto = ":trace_service_cc_proto",
    generate_mocks = True,
    no_undefined_ref_DO_NOT_USE = False,
    target_compatible_with = GRPC_TARGET_COMPATIBLE_WITH,
)

mongo_cc_library(
    name = "trace_service",
    srcs = [],
    hdrs = [],
    copts = OTEL_COPTS,
    target_compatible_with = GRPC_TARGET_COMPATIBLE_WITH,
    deps = [
        ":common_proto_cc",
        ":resource_proto_cc",
        ":trace_proto_cc",
        ":trace_service_cc_grpc",
        "//src/third_party/grpc:grpc++_reflection",
        "//src/third_party/protobuf",
    ],
)

mongo_proto_library(
    name = "metrics_proto",
    srcs = [
        "opentelemetry/proto/metrics/v1/metrics.proto",
    ],
    strip_import_prefix = "/src/third_party/opentelemetry-proto/",
    deps = [
        ":common_proto",
        ":resource_proto",
    ],
)

mongo_cc_proto_lib(
    name = "metrics_proto_cc",
    deps = [":metrics_proto"],
)

mongo_proto_library(
    name = "metrics_service_proto",
    srcs = [
        "opentelemetry/proto/collector/metrics/v1/metrics_service.proto",
    ],
    strip_import_prefix = "/src/third_party/opentelemetry-proto/",
    deps = [
        ":metrics_proto",
    ],
)

mongo_cc_proto_lib(
    name = "metrics_service_proto_cc_base",
    deps = [":metrics_service_proto"],
)

mongo_cc_library(
    name = "metrics_service_proto_cc",
    deps = select({
        "//bazel/config:build_grpc_enabled": [":metrics_service"],
        "//conditions:default": [":metrics_service_proto_cc_base"],
    }),
)
