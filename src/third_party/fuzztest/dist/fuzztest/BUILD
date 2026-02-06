# Copyright 2022 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# FuzzTest: a coverage-guided fuzzing / property-based testing framework.

load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

exports_files(["LICENSE"])

################################################################################
# Flag and setting to enable the Centipede integration.
################################################################################
bool_flag(
    name = "centipede_integration",
    build_setting_default = False,
)

config_setting(
    name = "use_centipede",
    flag_values = {":centipede_integration": "True"},
)
################################################################################

################################################################################
# Flag and setting to enable use of Riegeli
################################################################################
bool_flag(
    name = "use_riegeli",
    build_setting_default = True,
)

config_setting(
    name = "disable_riegeli",
    flag_values = {":use_riegeli": "False"},
)
################################################################################

cc_library(
    name = "fuzztest",
    hdrs = ["fuzztest.h"],
    deps = [
        ":domain",
        ":fuzztest_macros",
    ],
)

cc_library(
    name = "fuzztest_core",
    hdrs = ["fuzztest_core.h"],
    deps = [
        ":domain_core",
        ":fuzztest_macros",
    ],
)

cc_library(
    name = "fuzztest_macros",
    srcs = ["fuzztest_macros.cc"],
    hdrs = [
        "fuzztest_macros.h",
    ],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:string_view",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal:registration",
        "@com_google_fuzztest//fuzztest/internal:registry",
        "@com_google_fuzztest//fuzztest/internal:runtime",
    ],
)

cc_test(
    name = "fuzztest_macros_test",
    srcs = ["fuzztest_macros_test.cc"],
    deps = [
        ":fuzztest_macros",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:temp_dir",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "fuzztest_gtest_main",
    testonly = 1,
    srcs = ["fuzztest_gtest_main.cc"],
    deps = [
        "@com_google_fuzztest//fuzztest:init_fuzztest",
        "@googletest//:gtest",
    ],
)

cc_library(
    name = "init_fuzztest",
    testonly = 1,
    srcs = ["init_fuzztest.cc"],
    hdrs = ["init_fuzztest.h"],
    deps = [
        "@abseil-cpp//absl/algorithm:container",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/flags:reflection",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//fuzztest/internal:configuration",
        "@com_google_fuzztest//fuzztest/internal:flag_name",
        "@com_google_fuzztest//fuzztest/internal:googletest_adaptor",
        "@com_google_fuzztest//fuzztest/internal:io",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal:registry",
        "@com_google_fuzztest//fuzztest/internal:runtime",
        "@googletest//:gtest",
    ],
    alwayslink = True,
)

# TODO(hadi88): Add an e2e test for llvm_fuzzer_wrapper.
cc_library(
    name = "llvm_fuzzer_main",
    testonly = True,
    srcs = ["llvm_fuzzer_main.cc"],
    deps = [
        ":init_fuzztest",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@googletest//:gtest",
    ],
    alwayslink = True,
)

cc_library(
    name = "llvm_fuzzer_wrapper",
    testonly = True,
    srcs = ["llvm_fuzzer_wrapper.cc"],
    deps = [
        ":fuzztest",
        ":fuzztest_macros",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/log:absl_check",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/synchronization",
        "@com_google_fuzztest//fuzztest/internal:io",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal/domains:core_domains_impl",
    ],
    alwayslink = True,
)

cc_library(
    name = "domain",
    hdrs = ["domain.h"],
    deps = [
        ":domain_core",
        "@com_google_fuzztest//fuzztest/internal/domains:domains_impl",
        "@com_google_fuzztest//fuzztest/internal/domains:in_regexp_impl",
        "@com_google_fuzztest//fuzztest/internal/domains:protobuf_domain_impl",
    ],
)

# The core domain library without external dependencies e.g. re2. Mainly used
# by the default Centipede mutation.
cc_library(
    name = "domain_core",
    hdrs = ["domain_core.h"],
    deps = [
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:string_view",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest/internal:any",
        "@com_google_fuzztest//fuzztest/internal:logging",
        "@com_google_fuzztest//fuzztest/internal:meta",
        "@com_google_fuzztest//fuzztest/internal:printer",
        "@com_google_fuzztest//fuzztest/internal:serialization",
        "@com_google_fuzztest//fuzztest/internal:type_support",
        "@com_google_fuzztest//fuzztest/internal/domains:core_domains_impl",
        "@com_google_fuzztest//fuzztest/internal/domains:utf",
    ],
)

cc_library(
    name = "fuzzing_bit_gen",
    srcs = ["fuzzing_bit_gen.cc"],
    hdrs = ["fuzzing_bit_gen.h"],
    deps = [
        "@abseil-cpp//absl/base:fast_type_id",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/numeric:bits",
        "@abseil-cpp//absl/numeric:int128",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest/internal:register_fuzzing_mocks",
    ],
)

cc_library(
    name = "googletest_fixture_adapter",
    testonly = True,
    hdrs = ["googletest_fixture_adapter.h"],
    deps = [
        "@com_google_fuzztest//fuzztest/internal:fixture_driver",
        "@googletest//:gtest",
    ],
)
