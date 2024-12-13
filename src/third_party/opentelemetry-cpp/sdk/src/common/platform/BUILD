# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
load("//src/third_party/opentelemetry-cpp:otel_rules.bzl", "OTEL_TARGET_COMPATIBLE_WITH")

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "fork",
    srcs = select({
        "@platforms//os:windows": ["fork_windows.cc"],
        "//conditions:default": ["fork_unix.cc"],
    }),
    hdrs = [
        "fork.h",
    ],
    include_prefix = "src/common/platform",
    linkopts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-pthread"],
    }),
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        "//src/third_party/opentelemetry-cpp/api",
    ],
)
