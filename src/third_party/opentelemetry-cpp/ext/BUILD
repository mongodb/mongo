# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
load("//src/third_party/opentelemetry-cpp:otel_rules.bzl", "OTEL_COPTS", "OTEL_TARGET_COMPATIBLE_WITH")

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "headers",
    hdrs = glob(["include/**/*.h"]),
    copts = OTEL_COPTS,
    strip_include_prefix = "include",
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
)
