# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
load("//src/third_party/opentelemetry-cpp:otel_rules.bzl", "OTEL_COPTS", "OTEL_TARGET_COMPATIBLE_WITH")

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "in_memory_metric_data",
    testonly = True,
    srcs = [
        "src/in_memory_metric_data.cc",
    ],
    hdrs = [
        "include/opentelemetry/exporters/memory/in_memory_metric_data.h",
    ],
    strip_include_prefix = "include",
    tags = [
        "memory",
        "test",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    copts = OTEL_COPTS,
    deps = [
        ":in_memory_data",
        "//src/third_party/opentelemetry-cpp/sdk/src/metrics",
    ],
)

mongo_cc_library(
    name = "in_memory_metric_exporter_factory",
    testonly = True,
    srcs = [
        "src/in_memory_metric_exporter_factory.cc",
    ],
    hdrs = [
        "include/opentelemetry/exporters/memory/in_memory_metric_exporter_factory.h",
    ],
    strip_include_prefix = "include",
    tags = [
        "memory",
        "test",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    copts = OTEL_COPTS,
    deps = [
        ":in_memory_metric_data",
        "//src/third_party/opentelemetry-cpp/sdk/src/metrics",
    ],
)

mongo_cc_library(
    name = "in_memory_data",
    testonly = True,
    hdrs = [
        "include/opentelemetry/exporters/memory/in_memory_data.h",
    ],
    strip_include_prefix = "include",
    tags = ["memory"],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    copts = OTEL_COPTS,
    deps = [
        "//src/third_party/opentelemetry-cpp/sdk/src/metrics",
    ],
)

mongo_cc_library(
    name = "in_memory_span_data",
    testonly = True,
    hdrs = [
        "include/opentelemetry/exporters/memory/in_memory_span_data.h",
    ],
    strip_include_prefix = "include",
    tags = ["memory"],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    copts = OTEL_COPTS,
    deps = [
        ":in_memory_data",
        "//src/third_party/opentelemetry-cpp/api",
        "//src/third_party/opentelemetry-cpp/sdk/src/resource",
        "//src/third_party/opentelemetry-cpp/sdk/src/trace",
    ],
)

mongo_cc_library(
    name = "in_memory_span_exporter",
    testonly = True,
    srcs = [
        "src/in_memory_span_exporter_factory.cc",
    ],
    hdrs = [
        "include/opentelemetry/exporters/memory/in_memory_span_exporter.h",
        "include/opentelemetry/exporters/memory/in_memory_span_exporter_factory.h",
    ],
    strip_include_prefix = "include",
    tags = [
        "memory",
        "test",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    copts = OTEL_COPTS,
    deps = [
        ":in_memory_span_data",
        "//src/third_party/opentelemetry-cpp/sdk/src/trace",
    ],
)
