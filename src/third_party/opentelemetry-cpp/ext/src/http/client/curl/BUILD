# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
load("//src/third_party/opentelemetry-cpp:otel_rules.bzl", "OTEL_COPTS", "OTEL_TARGET_COMPATIBLE_WITH")

package(default_visibility = ["//visibility:public"])

mongo_cc_library(
    name = "http_client_curl",
    srcs = [
        "http_client_curl.cc",
        "http_client_factory_curl.cc",
        "http_operation_curl.cc",
    ],
    copts = [
        "-DWITH_CURL",
    ] + OTEL_COPTS,
    include_prefix = "src/http/client/curl",
    linkopts = select({
        "@platforms//os:windows": [
            "-DEFAULTLIB:advapi32.lib",
            "-DEFAULTLIB:crypt32.lib",
            "-DEFAULTLIB:Normaliz.lib",
            "-DEFAULTLIB:Ws2_32.lib",
        ],
        "//conditions:default": [
            "-lcurl",
        ],
    }),
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        "//src/third_party/opentelemetry-cpp/api",
        "//src/third_party/opentelemetry-cpp/ext:headers",
        "//src/third_party/opentelemetry-cpp/sdk:headers",
        "//src/third_party/opentelemetry-cpp/sdk/src/common:global_log_handler",
        "//src/third_party/opentelemetry-cpp/sdk/src/common:random",
        "//src/third_party/zlib",
    ],
)
