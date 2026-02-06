# Copyright 2024 Google LLC
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

load("@rules_proto//proto:defs.bzl", "proto_library")

package(default_visibility = ["@com_google_fuzztest//:__subpackages__"])

licenses(["notice"])

cc_library(
    name = "any",
    hdrs = ["any.h"],
    deps = [
        ":logging",
        ":meta",
    ],
)

cc_test(
    name = "any_test",
    srcs = ["any_test.cc"],
    deps = [
        ":any",
        "@abseil-cpp//absl/strings",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "centipede_adaptor",
    srcs = ["centipede_adaptor.cc"],
    hdrs = ["centipede_adaptor.h"],
    defines = ["FUZZTEST_USE_CENTIPEDE"],
    deps = [
        ":any",
        ":configuration",
        ":escaping",
        ":fixture_driver",
        ":flag_name",
        ":io",
        ":logging",
        ":runtime",
        ":subprocess",
        ":table_of_recent_compares",
        "@abseil-cpp//absl/algorithm:container",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/functional:any_invocable",
        "@abseil-cpp//absl/log:absl_log",
        "@abseil-cpp//absl/memory",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/strings:string_view",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//centipede:centipede_callbacks",
        "@com_google_fuzztest//centipede:centipede_default_callbacks",
        "@com_google_fuzztest//centipede:centipede_interface",
        "@com_google_fuzztest//centipede:centipede_runner_no_main",
        "@com_google_fuzztest//centipede:environment",
        "@com_google_fuzztest//centipede:execution_metadata",
        "@com_google_fuzztest//centipede:mutation_input",
        "@com_google_fuzztest//centipede:runner_result",
        "@com_google_fuzztest//centipede:stop",
        "@com_google_fuzztest//centipede:workdir",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:temp_dir",
        "@com_google_fuzztest//fuzztest/internal/domains:core_domains_impl",
    ],
)

cc_library(
    name = "compatibility_mode",
    srcs = ["compatibility_mode.cc"],
    hdrs = ["compatibility_mode.h"],
    deps = [
        ":fixture_driver",
        ":logging",
        ":runtime",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//fuzztest/internal/domains:core_domains_impl",
    ] + select({
        "//conditions:default": [],
    }),
)

cc_library(
    name = "configuration",
    srcs = ["configuration.cc"],
    hdrs = ["configuration.h"],
    deps = [
        "@abseil-cpp//absl/log:absl_check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:string_view",
        "@abseil-cpp//absl/time",
    ],
)

cc_test(
    name = "configuration_test",
    srcs = ["configuration_test.cc"],
    deps = [
        ":configuration",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/time",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "corpus_database",
    srcs = ["corpus_database.cc"],
    hdrs = ["corpus_database.h"],
    deps = [
        ":configuration",
        ":io",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:string_view",
        "@abseil-cpp//absl/time",
    ],
)

cc_library(
    name = "coverage",
    srcs = ["coverage.cc"],
    hdrs = ["coverage.h"],
    defines = select({
        "@com_google_fuzztest//fuzztest:use_centipede": ["FUZZTEST_USE_CENTIPEDE"],
        "//conditions:default": [],
    }),
    deps = [
        ":flag_name",
        ":logging",
        ":table_of_recent_compares",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest/internal/domains:core_domains_impl",
    ],
)

cc_library(
    name = "escaping",
    srcs = ["escaping.cc"],
    hdrs = ["escaping.h"],
    deps = ["@abseil-cpp//absl/strings"],
)

cc_library(
    name = "fixture_driver",
    srcs = ["fixture_driver.cc"],
    hdrs = ["fixture_driver.h"],
    deps = [
        ":any",
        ":logging",
        ":meta",
        ":printer",
        ":registration",
        ":type_support",
        "@abseil-cpp//absl/functional:any_invocable",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest/internal/domains:core_domains_impl",
    ],
)

cc_test(
    name = "fixture_driver_test",
    size = "small",
    srcs = ["fixture_driver_test.cc"],
    deps = [
        ":any",
        ":fixture_driver",
        ":logging",
        ":registration",
        "@abseil-cpp//absl/functional:any_invocable",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest:domain_core",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "flag_name",
    hdrs = ["flag_name.h"],
)

cc_library(
    name = "googletest_adaptor",
    testonly = True,
    srcs = ["googletest_adaptor.cc"],
    hdrs = ["googletest_adaptor.h"],
    deps = [
        ":configuration",
        ":corpus_database",
        ":flag_name",
        ":io",
        ":registry",
        ":runtime",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/strings:string_view",
        "@googletest//:gtest",
    ] + select({
        "@com_google_fuzztest//fuzztest:use_centipede": [":centipede_adaptor"],
        "//conditions:default": [],
    }),
)

cc_library(
    name = "io",
    srcs = ["io.cc"],
    hdrs = ["io.h"],
    deps = [
        ":logging",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/hash",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/strings:string_view",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:remote_file",
    ] + select({
        "//conditions:default": [],
    }),
)

cc_test(
    name = "io_test",
    srcs = ["io_test.cc"],
    deps = [
        ":io",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:temp_dir",
        "@com_google_fuzztest//fuzztest:fuzztest_core",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "logging",
    srcs = ["logging.cc"],
    hdrs = ["logging.h"],
    deps = [
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/synchronization",
    ],
)

cc_library(
    name = "meta",
    hdrs = ["meta.h"],
    deps = ["@abseil-cpp//absl/numeric:int128"],
)

cc_library(
    name = "printer",
    hdrs = ["printer.h"],
    deps = [
        ":meta",
        "@abseil-cpp//absl/strings:str_format",
    ],
)

cc_library(
    name = "register_fuzzing_mocks",
    srcs = ["register_fuzzing_mocks.cc"],
    hdrs = ["register_fuzzing_mocks.h"],
    deps = [
        "@abseil-cpp//absl/base:fast_type_id",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/log:absl_check",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/types:span",
    ],
)

cc_library(
    name = "registration",
    hdrs = ["registration.h"],
    deps = [
        ":meta",
        ":printer",
        ":type_support",
        "@abseil-cpp//absl/functional:any_invocable",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//fuzztest:domain_core",
        "@com_google_fuzztest//fuzztest/internal/domains:core_domains_impl",
    ],
)

cc_library(
    name = "registry",
    srcs = ["registry.cc"],
    hdrs = ["registry.h"],
    deps = [
        ":compatibility_mode",
        ":fixture_driver",
        ":registration",
        ":runtime",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/strings:string_view",
    ] + select({
        "@com_google_fuzztest//fuzztest:use_centipede": [":centipede_adaptor"],
        "//conditions:default": [],
    }),
)

cc_library(
    name = "runtime",
    srcs = ["runtime.cc"],
    hdrs = ["runtime.h"],
    deps = [
        ":configuration",
        ":corpus_database",
        ":coverage",
        ":fixture_driver",
        ":flag_name",
        ":io",
        ":logging",
        ":printer",
        ":registration",
        ":seed_seq",
        ":serialization",
        ":status",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/functional:any_invocable",
        "@abseil-cpp//absl/functional:bind_front",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/log:absl_check",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/random:distributions",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:bazel",
        "@com_google_fuzztest//fuzztest/internal/domains:core_domains_impl",
    ],
)

cc_test(
    name = "runtime_test",
    srcs = ["runtime_test.cc"],
    deps = [
        ":configuration",
        ":flag_name",
        ":runtime",
        ":test_protobuf_cc_proto",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//fuzztest:domain_core",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "seed_seq",
    srcs = ["seed_seq.cc"],
    hdrs = ["seed_seq.h"],
    deps = [
        ":logging",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/types:span",
    ],
)

cc_test(
    name = "seed_seq_test",
    srcs = ["seed_seq_test.cc"],
    deps = [
        ":seed_seq",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/types:span",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "serialization",
    srcs = ["serialization.cc"],
    hdrs = ["serialization.h"],
    deps = [
        ":meta",
        "@abseil-cpp//absl/numeric:int128",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/types:span",
    ],
)

cc_test(
    name = "serialization_test",
    srcs = ["serialization_test.cc"],
    deps = [
        ":serialization",
        ":test_protobuf_cc_proto",
        "@googletest//:gtest_main",
        "@protobuf",
    ],
)

cc_library(
    name = "status",
    srcs = ["status.cc"],
    hdrs = ["status.h"],
    deps = [
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:cord",
    ],
)

cc_library(
    name = "subprocess",
    srcs = ["subprocess.cc"],
    hdrs = ["subprocess.h"],
    deps = [
        ":logging",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:span",
    ],
)

cc_test(
    name = "subprocess_test",
    srcs = ["subprocess_test.cc"],
    deps = [
        ":subprocess",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "table_of_recent_compares",
    hdrs = ["table_of_recent_compares.h"],
    deps = [
        ":type_support",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/random:bit_gen_ref",
        "@abseil-cpp//absl/random:distributions",
    ],
)

cc_test(
    name = "table_of_recent_compares_test",
    srcs = ["table_of_recent_compares_test.cc"],
    deps = [
        ":table_of_recent_compares",
        "@abseil-cpp//absl/random",
        "@googletest//:gtest_main",
    ],
)

proto_library(
    name = "test_protobuf",
    srcs = ["test_protobuf.proto"],
)

cc_proto_library(
    name = "test_protobuf_cc_proto",
    deps = [":test_protobuf"],
)

cc_library(
    name = "type_support",
    srcs = ["type_support.cc"],
    hdrs = ["type_support.h"],
    deps = [
        ":meta",
        ":printer",
        "@abseil-cpp//absl/debugging:symbolize",
        "@abseil-cpp//absl/functional:function_ref",
        "@abseil-cpp//absl/numeric:int128",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//fuzztest/internal/domains:absl_helpers",
    ],
)

cc_test(
    name = "type_support_test",
    srcs = ["type_support_test.cc"],
    deps = [
        ":meta",
        ":printer",
        ":test_protobuf_cc_proto",
        ":type_support",
        "@abseil-cpp//absl/numeric:int128",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//fuzztest:domain",
        "@googletest//:gtest_main",
        "@protobuf",
    ],
)
