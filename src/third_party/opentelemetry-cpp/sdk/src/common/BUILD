# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
load("//src/third_party/opentelemetry-cpp:otel_rules.bzl", "OTEL_TARGET_COMPATIBLE_WITH")

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "random",
    srcs = [
        "random.cc",
    ],
    hdrs = [
        "fast_random_number_generator.h",
        "random.h",
    ],
    include_prefix = "src/common",
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        "//src/third_party/opentelemetry-cpp/api",
        "//src/third_party/opentelemetry-cpp/sdk:headers",
        "//src/third_party/opentelemetry-cpp/sdk/src/common/platform:fork",
    ],
)

mongo_cc_library(
    name = "base64",
    srcs = [
        "base64.cc",
    ],
    include_prefix = "src/common",
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        "//src/third_party/opentelemetry-cpp/api",
        "//src/third_party/opentelemetry-cpp/sdk:headers",
        "//src/third_party/opentelemetry-cpp/sdk/src/common/platform:fork",
    ],
)

mongo_cc_library(
    name = "env_variables",
    srcs = [
        "env_variables.cc",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        "//src/third_party/opentelemetry-cpp/api",
        "//src/third_party/opentelemetry-cpp/sdk:headers",
        "//src/third_party/opentelemetry-cpp/sdk/src/common:global_log_handler",
    ],
)

mongo_cc_library(
    name = "global_log_handler",
    srcs = [
        "global_log_handler.cc",
    ],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        "//src/third_party/opentelemetry-cpp/api",
        "//src/third_party/opentelemetry-cpp/sdk:headers",
    ],
)
