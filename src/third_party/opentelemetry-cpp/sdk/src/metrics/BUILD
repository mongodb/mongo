# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "metrics",
    srcs = glob(["**/*.cc"]),
    hdrs = glob(["**/*.h"]),
    include_prefix = "src/metrics",
    deps = [
        "//src/third_party/opentelemetry-cpp/api",
        "//src/third_party/opentelemetry-cpp/sdk:headers",
        "//src/third_party/opentelemetry-cpp/sdk/src/common:global_log_handler",
        "//src/third_party/opentelemetry-cpp/sdk/src/common:random",
        "//src/third_party/opentelemetry-cpp/sdk/src/resource",
    ],
)
