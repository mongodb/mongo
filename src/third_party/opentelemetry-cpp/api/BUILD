# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

load("//bazel:mongo_src_rules.bzl", "mongo_cc_library")
load("@bazel_skylib//rules:common_settings.bzl", "bool_flag", "int_flag", "string_flag")
load("//src/third_party/opentelemetry-cpp:otel_rules.bzl", "OTEL_COPTS", "OTEL_TARGET_COMPATIBLE_WITH")

package(default_visibility = ["//visibility:public"])

CPP_STDLIBS = [
    "none",
    "best",
    "2014",
    "2017",
    "2020",
    "2023",
]

string_flag(
    name = "with_cxx_stdlib",
    build_setting_default = "best",
    values = CPP_STDLIBS,
)

mongo_cc_library(
    name = "api",
    hdrs = glob(["include/**/*.h"]),
    copts = OTEL_COPTS,
    defines = ["HAVE_ABSEIL"] + select({
        ":set_cxx_stdlib_none": [],
        ### automatic selection
        ":set_cxx_stdlib_best": ["OPENTELEMETRY_STL_VERSION=(__cplusplus/100)"],
        # See https://learn.microsoft.com/en-us/cpp/build/reference/zc-cplusplus
        ":set_cxx_stdlib_best_and_msvc": ["OPENTELEMETRY_STL_VERSION=(_MSVC_LANG/100)"],
        ### manual selection
        ":set_cxx_stdlib_2014": ["OPENTELEMETRY_STL_VERSION=2014"],
        ":set_cxx_stdlib_2017": ["OPENTELEMETRY_STL_VERSION=2017"],
        ":set_cxx_stdlib_2020": ["OPENTELEMETRY_STL_VERSION=2020"],
        ":set_cxx_stdlib_2023": ["OPENTELEMETRY_STL_VERSION=2023"],
        "//conditions:default": [],
    }) + select({
        ":abi_version_no_1": ["OPENTELEMETRY_ABI_VERSION_NO=1"],
        ":abi_version_no_2": ["OPENTELEMETRY_ABI_VERSION_NO=2"],
    }),
    strip_include_prefix = "include",
    tags = ["api"],
    target_compatible_with = OTEL_TARGET_COMPATIBLE_WITH,
    deps = [
        "//src/third_party/abseil-cpp:absl_base",
        "//src/third_party/abseil-cpp:absl_strings",
    ],
)

[config_setting(
    name = "set_cxx_stdlib_%s" % v,
    flag_values = {":with_cxx_stdlib": v},
) for v in CPP_STDLIBS]

config_setting(
    name = "set_cxx_stdlib_best_and_msvc",
    constraint_values = ["@bazel_tools//tools/cpp:msvc"],
    flag_values = {":with_cxx_stdlib": "best"},
)

bool_flag(
    name = "with_abseil",
    build_setting_default = False,
    deprecation = "The value of this flag is ignored. Bazel builds always depend on Abseil for its pre-adopted `std::` types. You should remove this flag from your build command.",
)

int_flag(
    name = "abi_version_no",
    build_setting_default = 1,
)

config_setting(
    name = "abi_version_no_1",
    flag_values = {":abi_version_no": "1"},
)

config_setting(
    name = "abi_version_no_2",
    flag_values = {":abi_version_no": "2"},
)
