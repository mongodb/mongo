# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
load("//src/third_party/opentelemetry-cpp:otel_rules.bzl", "OTEL_TARGET_COMPATIBLE_WITH")

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "trace",
    srcs = glob(["**/*.cc"]),
    hdrs = glob(["**/*.h"]),
    include_prefix = "src/trace",
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        "//src/third_party/opentelemetry-cpp/api",
        "//src/third_party/opentelemetry-cpp/sdk:headers",
        "//src/third_party/opentelemetry-cpp/sdk/src/common:global_log_handler",
        "//src/third_party/opentelemetry-cpp/sdk/src/common:random",
        "//src/third_party/opentelemetry-cpp/sdk/src/resource",
    ],
)
