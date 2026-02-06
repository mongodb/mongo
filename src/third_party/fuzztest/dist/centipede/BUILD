# Copyright 2022 The Centipede Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Description:
#   Centipede: an experimental distributed fuzzing engine.

load("@rules_proto//proto:defs.bzl", "proto_library")
load(":instrument.bzl", "cc_uninstrumented_binary")

VISIBILITY = ["//visibility:public"]

EXTENDED_API_VISIBILITY = VISIBILITY

PUBLIC_API_VISIBILITY = VISIBILITY

package(default_visibility = VISIBILITY)

licenses(["notice"])

################################################################################
#                                  Binaries
################################################################################

cc_binary(
    name = "centipede",
    srcs = ["centipede_main.cc"],
    deps = [
        ":centipede_callbacks",
        ":centipede_default_callbacks",
        ":centipede_interface",
        ":config_file",
        ":environment_flags",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/log:flags",
    ],
)

cc_uninstrumented_binary(
    name = "centipede_uninstrumented",
    binary = ":centipede",
    visibility = PUBLIC_API_VISIBILITY,
)

# A standalone seed corpus generator.
cc_binary(
    name = "seed_corpus_maker",
    srcs = ["seed_corpus_maker.cc"],
    deps = [
        ":config_init",
        ":seed_corpus_maker_flags",
        ":seed_corpus_maker_proto_lib",
        ":util",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@com_google_fuzztest//common:remote_file",
    ],
)

cc_binary(
    name = "blob_file_converter",
    srcs = ["blob_file_converter.cc"],
    deps = [
        ":config_init",
        ":rusage_profiler",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
    ],
)

###############################################################################
#                              Proto libraries
###############################################################################

# Configuration for the seed corpus maker.
proto_library(
    name = "seed_corpus_config_proto",
    srcs = ["seed_corpus_config.proto"],
)

cc_proto_library(
    name = "seed_corpus_config_cc_proto",
    visibility = EXTENDED_API_VISIBILITY,
    deps = [":seed_corpus_config_proto"],
)

################################################################################
#                             C++ libraries
################################################################################

# This lib must have zero non-trivial dependencies (other than libc).
cc_library(
    name = "int_utils",
    hdrs = ["int_utils.h"],
)

cc_library(
    name = "rolling_hash",
    hdrs = ["rolling_hash.h"],
)

cc_library(
    name = "symbol_table",
    srcs = ["symbol_table.cc"],
    hdrs = ["symbol_table.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":command",
        ":control_flow",
        ":pc_info",
        ":thread_pool",
        ":util",
        "@abseil-cpp//absl/container:node_hash_set",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:logging",
    ],
)

cc_library(
    name = "foreach_nonzero",
    hdrs = ["foreach_nonzero.h"],
)

# This lib must have zero non-trivial dependencies (other than libc). See feature.h.
cc_library(
    name = "feature",
    srcs = ["feature.cc"],
    hdrs = [
        "concurrent_bitset.h",  # TODO(kcc): consider moving to a separate cc_library.
        "concurrent_byteset.h",  # TODO(kcc): consider moving to a separate cc_library.
        "feature.h",
        "hashed_ring_buffer.h",  # TODO(kcc): consider moving to a separate cc_library.
    ],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":rolling_hash",
        "@abseil-cpp//absl/base:core_headers",  # exception, ok to depend on here.
    ],
)

cc_library(
    name = "pc_info",
    srcs = ["pc_info.cc"],
    hdrs = ["pc_info.h"],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:defs",
    ],
)

cc_library(
    name = "knobs",
    srcs = ["knobs.cc"],
    hdrs = ["knobs.h"],
    # Avoid non-trivial dependencies here, as this library will be linked to target binaries.
    deps = [
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:defs",
    ],
)

cc_library(
    name = "reverse_pc_table",
    hdrs = ["reverse_pc_table.h"],
    # Avoid non-trivial dependencies here, as this library will be linked to target binaries.
    deps = [":pc_info"],
)

cc_library(
    name = "callstack",
    hdrs = ["callstack.h"],
    # Avoid non-trivial dependencies here, as this library will be linked to target binaries.
    deps = [
        ":rolling_hash",
    ],
)

# Various utilities.
cc_library(
    name = "util",
    srcs = ["util.cc"],
    hdrs = ["util.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":feature",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
    ],
)

cc_library(
    name = "config_util",
    srcs = ["config_util.cc"],
    hdrs = ["config_util.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        "@abseil-cpp//absl/flags:reflection",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "config_init",
    srcs = ["config_init.cc"],
    hdrs = ["config_init.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":config_util",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:log_severity",
        "@abseil-cpp//absl/flags:config",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/log:globals",
        "@abseil-cpp//absl/log:initialize",
        "@abseil-cpp//absl/strings",
    ],
)

cc_library(
    name = "config_file",
    srcs = ["config_file.cc"],
    hdrs = ["config_file.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":config_init",
        ":config_util",
        ":util",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/flags:parse",
        "@abseil-cpp//absl/flags:reflection",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
    ],
)

cc_library(
    name = "thread_pool",
    hdrs = ["thread_pool.h"],
    deps = [
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/functional:any_invocable",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/synchronization",
    ],
)

cc_library(
    name = "rusage_stats",
    srcs = ["rusage_stats.cc"],
    hdrs = ["rusage_stats.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
    ],
)

cc_library(
    name = "rusage_profiler",
    srcs = ["rusage_profiler.cc"],
    hdrs = ["rusage_profiler.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":periodic_action",
        ":rusage_stats",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
    ],
)

cc_library(
    name = "resource_pool",
    srcs = ["resource_pool.cc"],
    hdrs = ["resource_pool.h"],
    deps = [
        ":rusage_stats",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
    ],
)

cc_library(
    name = "periodic_action",
    srcs = ["periodic_action.cc"],
    hdrs = ["periodic_action.h"],
    deps = [
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/functional:any_invocable",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
    ],
)

cc_library(
    name = "stats",
    srcs = ["stats.cc"],
    hdrs = ["stats.h"],
    linkopts = select({
        "@platforms//os:macos": [],
        "//conditions:default": [
            "-latomic",  # for std::atomic::load()/store().
        ],
    }),
    deps = [
        ":environment",
        ":workdir",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/container:btree",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
    ],
)

cc_library(
    name = "minimize_crash",
    srcs = ["minimize_crash.cc"],
    hdrs = ["minimize_crash.h"],
    deps = [
        ":centipede_callbacks",
        ":environment",
        ":mutation_input",
        ":runner_result",
        ":stop",
        ":thread_pool",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/synchronization",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@com_google_fuzztest//common:logging",
    ],
)

cc_library(
    name = "analyze_corpora",
    srcs = ["analyze_corpora.cc"],
    hdrs = ["analyze_corpora.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":binary_info",
        ":control_flow",
        ":corpus",
        ":corpus_io",
        ":coverage",
        ":feature",
        ":pc_info",
        ":workdir",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
    ],
)

cc_library(
    name = "shared_memory_blob_sequence",
    srcs = ["shared_memory_blob_sequence.cc"],
    hdrs = ["shared_memory_blob_sequence.h"],
    linkopts = select({
        "@platforms//os:macos": [],
        "//conditions:default": [
            "-lrt",  # for shm_open
        ],
    }),
    visibility = PUBLIC_API_VISIBILITY,
    deps = ["@abseil-cpp//absl/base:nullability"],
    # don't add any dependencies.
)

cc_library(
    name = "execution_metadata",
    srcs = ["execution_metadata.cc"],
    hdrs = ["execution_metadata.h"],
    deps = [
        # This target must have a minimal set of dependencies since it is
        # used in centipede_runner.
        "@com_google_fuzztest//common:defs",
        ":shared_memory_blob_sequence",
    ],
)

cc_library(
    name = "runner_result",
    srcs = ["runner_result.cc"],
    hdrs = ["runner_result.h"],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        # This target must have a minimal set of dependencies since it is
        # used in centipede_runner.
        ":feature",
        ":execution_metadata",
        ":shared_memory_blob_sequence",
        "@com_google_fuzztest//common:defs",
    ],
)

cc_library(
    name = "runner_request",
    srcs = ["runner_request.cc"],
    hdrs = ["runner_request.h"],
    deps = [
        # This target must have a minimal set of dependencies since it is
        # used in centipede_runner.
        ":shared_memory_blob_sequence",
        ":execution_metadata",
        ":mutation_input",
        "@com_google_fuzztest//common:defs",
    ],
)

cc_library(
    name = "mutation_input",
    hdrs = ["mutation_input.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        # This target must have a minimal set of dependencies since it is
        # used in centipede_runner.
        "@com_google_fuzztest//common:defs",
        ":execution_metadata",
    ],
)

cc_library(
    name = "byte_array_mutator",
    srcs = ["byte_array_mutator.cc"],
    hdrs = ["byte_array_mutator.h"],
    visibility = PUBLIC_API_VISIBILITY,
    # Avoid non-trivial dependencies here, as this library will be linked to target binaries.
    deps = [
        ":execution_metadata",
        ":knobs",
        ":mutation_input",
        "@abseil-cpp//absl/base:nullability",
        "@com_google_fuzztest//common:defs",
    ],
)

# Library for dealing with code coverage data from
# https://clang.llvm.org/docs/SanitizerCoverage.html.
cc_library(
    name = "coverage",
    srcs = [
        "coverage.cc",
    ],
    hdrs = [
        "coverage.h",
    ],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":control_flow",
        ":feature",
        ":pc_info",
        ":symbol_table",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/synchronization",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:status_macros",
    ],
)

# Library for dealing with control flow data from
# https://clang.llvm.org/docs/SanitizerCoverage.html#tracing-control-flow.
cc_library(
    name = "control_flow",
    srcs = [
        "control_flow.cc",
    ],
    hdrs = [
        "control_flow.h",
    ],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":command",
        ":pc_info",
        ":util",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
    ],
)

# Library for dealing with call graph data from
# https://clang.llvm.org/docs/SanitizerCoverage.html#tracing-control-flow.
cc_library(
    name = "call_graph",
    srcs = [
        "call_graph.cc",
    ],
    hdrs = [
        "call_graph.h",
    ],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":control_flow",
        ":pc_info",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log:check",
        "@com_google_fuzztest//common:logging",
    ],
)

cc_library(
    name = "feature_set",
    srcs = ["feature_set.cc"],
    hdrs = ["feature_set.h"],
    deps = [
        ":control_flow",
        ":feature",
        ":util",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:logging",
    ],
)

# TODO(kcc): [impl] add dedicated unittests.
cc_library(
    name = "corpus",
    srcs = ["corpus.cc"],
    hdrs = ["corpus.h"],
    deps = [
        ":binary_info",
        ":control_flow",
        ":coverage",
        ":execution_metadata",
        ":feature",
        ":feature_set",
        ":util",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:status_macros",
    ],
)

cc_library(
    name = "command",
    srcs = ["command.cc"],
    hdrs = ["command.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":stop",
        ":util",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:logging",
    ],
)

cc_library(
    name = "binary_info",
    srcs = ["binary_info.cc"],
    hdrs = [
        "binary_info.h",
    ],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":call_graph",
        ":command",
        ":control_flow",
        ":pc_info",
        ":symbol_table",
        ":util",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:remote_file",
    ],
)

cc_library(
    name = "centipede_callbacks",
    srcs = [
        "centipede_callbacks.cc",
    ],
    hdrs = [
        "centipede_callbacks.h",
    ],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":binary_info",
        ":byte_array_mutator",
        ":command",
        ":control_flow",
        ":environment",
        ":fuzztest_mutator",
        ":mutation_input",
        ":runner_request",
        ":runner_result",
        ":shared_memory_blob_sequence",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@com_google_fuzztest//common:logging",
    ],
)

cc_library(
    name = "corpus_io",
    srcs = ["corpus_io.cc"],
    hdrs = ["corpus_io.h"],
    deps = [
        ":feature",
        ":rusage_profiler",
        ":util",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
    ],
)

cc_library(
    name = "stop",
    srcs = ["stop.cc"],
    hdrs = ["stop.h"],
    linkopts = select({
        "@platforms//os:macos": [],
        "//conditions:default": [
            "-latomic",  # for std::atomic::load()/store().
        ],
    }),
    visibility = PUBLIC_API_VISIBILITY,
    deps = ["@abseil-cpp//absl/time"],
)

cc_library(
    name = "environment_flags",
    srcs = ["environment_flags.cc"],
    hdrs = ["environment_flags.h"],
    defines = select({
        "@com_google_fuzztest//fuzztest:disable_riegeli": ["CENTIPEDE_DISABLE_RIEGELI"],
        "//conditions:default": [],
    }),
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":centipede_flags",
        ":environment",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:logging",
    ],
)

cc_library(
    name = "centipede_flags",
    textual_hdrs = ["centipede_flags.inc"],
)

cc_library(
    name = "centipede_lib",
    srcs = [
        "centipede.cc",
    ],
    hdrs = [
        "centipede.h",
    ],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":binary_info",
        ":centipede_callbacks",
        ":command",
        ":control_flow",
        ":corpus",
        ":corpus_io",
        ":coverage",
        ":environment",
        ":feature",
        ":feature_set",
        ":mutation_input",
        ":pc_info",
        ":runner_result",
        ":rusage_profiler",
        ":rusage_stats",
        ":stats",
        ":stop",
        ":symbol_table",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:status_macros",
    ],
)

cc_library(
    name = "centipede_interface",
    srcs = [
        "centipede_interface.cc",
    ],
    hdrs = [
        "centipede_interface.h",
    ],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":analyze_corpora",
        ":binary_info",
        ":centipede_callbacks",
        ":centipede_lib",
        ":command",
        ":coverage",
        ":crash_summary",
        ":distill",
        ":environment",
        ":minimize_crash",
        ":pc_info",
        ":periodic_action",
        ":runner_result",
        ":seed_corpus_maker_lib",
        ":stats",
        ":stop",
        ":thread_pool",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/cleanup",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:bazel",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:status_macros",
        "@com_google_fuzztest//fuzztest/internal:configuration",
    ] + select({
        "//conditions:default": [],
    }),
)

cc_library(
    name = "environment",
    srcs = [
        "environment.cc",
    ],
    hdrs = [
        "environment.h",
    ],
    defines = select({
        "@com_google_fuzztest//fuzztest:disable_riegeli": ["CENTIPEDE_DISABLE_RIEGELI"],
        "//conditions:default": [],
    }),
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":centipede_flags",
        ":feature",
        ":knobs",
        ":util",
        "@abseil-cpp//absl/base:no_destructor",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/flags:marshalling",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:status_macros",
        "@com_google_fuzztest//fuzztest/internal:configuration",
    ],
)

cc_library(
    name = "distill",
    srcs = ["distill.cc"],
    hdrs = ["distill.h"],
    deps = [
        ":corpus_io",
        ":environment",
        ":feature",
        ":feature_set",
        ":periodic_action",
        ":resource_pool",
        ":rusage_profiler",
        ":rusage_stats",
        ":thread_pool",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/base:core_headers",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:status_macros",
    ],
)

cc_library(
    name = "centipede_default_callbacks",
    srcs = ["centipede_default_callbacks.cc"],
    hdrs = ["centipede_default_callbacks.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":centipede_callbacks",
        ":environment",
        ":mutation_input",
        ":runner_result",
        ":stop",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:logging",
    ],
)

cc_library(
    name = "fuzztest_mutator",
    srcs = ["fuzztest_mutator.cc"],
    hdrs = ["fuzztest_mutator.h"],
    deps = [
        ":byte_array_mutator",
        ":execution_metadata",
        ":knobs",
        ":mutation_input",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//fuzztest:domain_core",
        "@com_google_fuzztest//fuzztest/internal:table_of_recent_compares",
    ],
)

cc_library(
    name = "crash_summary",
    srcs = ["crash_summary.cc"],
    hdrs = ["crash_summary.h"],
    deps = [
        ":util",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:defs",
    ],
)

cc_library(
    name = "weak_sancov_stubs",
    srcs = ["weak_sancov_stubs.cc"],
    visibility = ["//visibility:public"],
    alwayslink = 1,
)

# runner_fork_server can be linked to a binary or used directly as a .so via LD_PRELOAD.
cc_library(
    name = "runner_fork_server",
    srcs = ["runner_fork_server.cc"],
    visibility = PUBLIC_API_VISIBILITY,
    deps = ["@abseil-cpp//absl/base:nullability"],
    alwayslink = 1,  # Otherwise the linker drops the fork server.
)

cc_library(
    name = "runner_cmp_trace",
    hdrs = ["runner_cmp_trace.h"],
)

# Library for manipulating centipede runner flags. This is not used by the
# runner itself but by other tools.
cc_library(
    name = "runner_flags",
    srcs = ["runner_flags.cc"],
    hdrs = ["runner_flags.h"],
    visibility = PUBLIC_API_VISIBILITY,
    # This has no dependency and only uses string, pair and vector.
)

cc_library(
    name = "dispatcher",
    srcs = ["dispatcher.cc"],
    hdrs = ["dispatcher.h"],
    deps = [
        ":execution_metadata",
        ":runner_request",
        ":runner_result",
        ":shared_memory_blob_sequence",
        "@abseil-cpp//absl/base:nullability",
        "@com_google_fuzztest//common:defs",
    ],
)

# The runner library is special:
#   * It must not be instrumented with asan, sancov, etc.
#   * It must not have heavy dependencies, and ideally not at all.
#     Exceptions are STL and absl::span (temporarily, until we can switch to
#     std::span).
#   * The bazel rule :centipede_runner must produce a self-contained .a file
#     with all
#     objects in it, which means the build rule must not depend other .a rules.
#
#  Some of the .cc and .h files used by the runner are also used by the engine,
#  e.g. feature.cc. These files are compiled by the engine and the runner
#  separately, with different compiler flags.
RUNNER_SOURCES_NO_MAIN = [
    "byte_array_mutator.cc",
    "byte_array_mutator.h",
    "callstack.h",
    "concurrent_bitset.h",
    "concurrent_byteset.h",
    "execution_metadata.cc",
    "execution_metadata.h",
    "runner_request.cc",
    "runner_request.h",
    "runner_result.cc",
    "runner_result.h",
    "feature.cc",
    "feature.h",
    "foreach_nonzero.h",
    "hashed_ring_buffer.h",
    "int_utils.h",
    "knobs.h",
    "knobs.cc",
    "mutation_input.h",
    "pc_info.h",
    "reverse_pc_table.h",
    "rolling_hash.h",
    "runner.cc",
    "runner.h",
    "runner_dl_info.h",
    "runner_dl_info.cc",
    "runner_sancov_object.h",
    "runner_sancov_object.cc",
    "runner_utils.h",
    "runner_utils.cc",
    "runner_cmp_trace.h",
    "runner_fork_server.cc",
    "runner_interceptors.cc",
    "runner_interface.h",
    "runner_sancov.cc",
    "shared_memory_blob_sequence.cc",
    "shared_memory_blob_sequence.h",
    "@com_google_fuzztest//common:defs.h",
]

RUNNER_SOURCES_WITH_MAIN = RUNNER_SOURCES_NO_MAIN + ["runner_main.cc"]

# Disable sancov and sanitizer instrumentation
RUNNER_COPTS = [
    "-fsanitize-coverage=0",
    "-fno-sanitize=address,hwaddress,memory,thread,undefined",
]

RUNNER_SANITIZED_COPTS = [
    "-fsanitize-coverage=0",
]

RUNNER_LINKOPTS = [
    "-ldl",  # for dlsym
    "-lpthread",  # for pthread_once
] + select({
    "@platforms//os:macos": [
        # For compatible behavior on weak symbols.
        "-Wl,-undefined,dynamic_lookup",
    ],
    "//conditions:default": [
        "-lrt",  # for shm_open
    ],
})

# WARNING: be careful with more deps here. Use only the most trivial ones.
RUNNER_DEPS = [
    "@abseil-cpp//absl/base:nullability",
    "@abseil-cpp//absl/types:span",
    "@abseil-cpp//absl/base:core_headers",
    "@abseil-cpp//absl/numeric:bits",
]

# A fuzz target needs to link with this library in order to run with Centipede.
# The fuzz target must provide its own main().
#
# See also comments above RUNNER_SOURCES_NO_MAIN.
#
cc_library(
    name = "centipede_runner_no_main",
    srcs = RUNNER_SOURCES_NO_MAIN,
    hdrs = ["runner_interface.h"],
    copts = RUNNER_COPTS,
    linkopts = RUNNER_LINKOPTS,
    linkstatic = True,  # Must be linked statically even when dynamic_mode=on.
    visibility = ["//visibility:public"],
    deps = RUNNER_DEPS,
)

# Same as :centipede_runner_no_main, except it can be sanitized to avoid false
# positives like
# https://github.com/google/sanitizers/wiki/AddressSanitizerContainerOverflow#false-positives
cc_library(
    name = "centipede_runner_no_main_sanitized",
    srcs = RUNNER_SOURCES_NO_MAIN,
    hdrs = ["runner_interface.h"],
    copts = RUNNER_SANITIZED_COPTS,
    linkopts = RUNNER_LINKOPTS,
    linkstatic = True,  # Must be linked statically even when dynamic_mode=on.
    visibility = ["//visibility:public"],
    deps = RUNNER_DEPS,
)

# Same as :centipede_runner_no_main but as a DSO. Experimental.
cc_binary(
    name = "centipede_runner_no_main.so",
    srcs = RUNNER_SOURCES_NO_MAIN,
    copts = RUNNER_COPTS,
    linkopts = RUNNER_LINKOPTS,
    linkshared = 1,
    visibility = ["//visibility:public"],
    deps = RUNNER_DEPS,
)

# A full self-contained library archive that external clients should link to
# their fuzz targets to make them compatible with the Centipede fuzzing engine
# (the `:centipede` target in this BUILD).
cc_library(
    name = "centipede_runner",
    srcs = RUNNER_SOURCES_WITH_MAIN,
    hdrs = ["runner_interface.h"],
    copts = RUNNER_COPTS,
    linkopts = RUNNER_LINKOPTS,
    linkstatic = True,  # Must be linked statically even when dynamic_mode=on.
    visibility = ["//visibility:public"],
    deps = RUNNER_DEPS,
)

# Utilities for seed corpus generation.
cc_library(
    name = "seed_corpus_maker_lib",
    srcs = ["seed_corpus_maker_lib.cc"],
    hdrs = ["seed_corpus_maker_lib.h"],
    visibility = EXTENDED_API_VISIBILITY,
    deps = [
        ":corpus_io",
        ":feature",
        ":rusage_profiler",
        ":thread_pool",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/random",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:status_macros",
    ],
)

# Proto-related utilities for the seed corpus maker.
cc_library(
    name = "seed_corpus_maker_proto_lib",
    srcs = ["seed_corpus_maker_proto_lib.cc"],
    hdrs = ["seed_corpus_maker_proto_lib.h"],
    deps = [
        ":seed_corpus_config_cc_proto",
        ":seed_corpus_maker_lib",
        ":workdir",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:status_macros",
        "@protobuf",
    ],
)

# Flags for :seed_corpus_maker.
cc_library(
    name = "seed_corpus_maker_flags",
    srcs = ["seed_corpus_maker_flags.cc"],
    hdrs = ["seed_corpus_maker_flags.h"],
    deps = ["@abseil-cpp//absl/flags:flag"],
)

################################################################################
#                        General-purpose testing utilities
################################################################################

cc_library(
    name = "workdir",
    srcs = ["workdir.cc"],
    hdrs = ["workdir.h"],
    visibility = PUBLIC_API_VISIBILITY,
    deps = [
        ":environment",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/strings:str_format",
        "@com_google_fuzztest//common:logging",
    ],
)

cc_library(
    name = "test_coverage_util",
    testonly = True,
    srcs = ["test_coverage_util.cc"],
    hdrs = ["test_coverage_util.h"],
    deps = [
        ":centipede_callbacks",
        ":corpus",
        ":environment",
        ":feature",
        ":mutation_input",
        ":runner_result",
        ":util",
        "@abseil-cpp//absl/log:check",
        "@com_google_fuzztest//common:defs",
    ],
)

sh_library(
    name = "test_fuzzing_util_sh",
    srcs = ["test_fuzzing_util.sh"],
)

sh_library(
    name = "test_util_sh",
    srcs = ["test_util.sh"],
)

################################################################################
#                               Unit tests
################################################################################

cc_test(
    name = "binary_info_test",
    srcs = ["binary_info_test.cc"],
    deps = [
        ":binary_info",
        ":control_flow",
        ":pc_info",
        ":symbol_table",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "environment_test",
    srcs = ["environment_test.cc"],
    deps = [
        ":environment",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//fuzztest/internal:configuration",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "workdir_test",
    srcs = ["workdir_test.cc"],
    deps = [
        ":environment",
        ":workdir",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "pc_info_test",
    srcs = ["pc_info_test.cc"],
    deps = [
        ":pc_info",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "rolling_hash_test",
    srcs = ["rolling_hash_test.cc"],
    deps = [
        ":feature",
        ":rolling_hash",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "symbol_table_test",
    srcs = ["symbol_table_test.cc"],
    deps = [
        ":symbol_table",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "util_test",
    srcs = ["util_test.cc"],
    copts = ["-fno-signed-char"],
    deps = [
        ":feature",
        ":thread_pool",
        ":util",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/log",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "config_util_test",
    srcs = ["config_util_test.cc"],
    deps = [
        # Include this just to get some flags external to the test itself.
        ":environment",  # buildcleaner:keep
        ":config_util",
        ":environment_flags",
        "@com_google_fuzztest//common:logging",
        "@googletest//:gtest_main",
        "@abseil-cpp//absl/flags:flag",
    ],
)

cc_test(
    name = "config_file_test",
    srcs = ["config_file_test.cc"],
    deps = [
        ":config_file",
        "@googletest//:gtest_main",
        # Defines FLAGS_flagfile.
        "@abseil-cpp//absl/flags:parse",  # buildcleaner:keep
    ],
)

cc_test(
    name = "rusage_stats_test",
    size = "medium",
    timeout = "long",
    srcs = ["rusage_stats_test.cc"],
    deps = [
        ":rusage_stats",
        ":thread_pool",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:logging",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "distill_test",
    srcs = ["distill_test.cc"],
    deps = [
        ":corpus_io",
        ":distill",
        ":environment",
        ":feature",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/flags:reflection",
        "@abseil-cpp//absl/log:check",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "rusage_profiler_test",
    # Allocates large blocks of memory to fight small number volatility.
    size = "large",
    timeout = "long",
    srcs = ["rusage_profiler_test.cc"],
    deps = [
        ":rusage_profiler",
        ":rusage_stats",
        "@abseil-cpp//absl/flags:flag",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/time",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "stats_test",
    srcs = ["stats_test.cc"],
    deps = [
        ":environment",
        ":stats",
        ":util",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/log:log_entry",
        "@abseil-cpp//absl/log:log_sink",
        "@abseil-cpp//absl/log:log_sink_registry",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "minimize_crash_test",
    srcs = ["minimize_crash_test.cc"],
    deps = [
        ":centipede_callbacks",
        ":environment",
        ":minimize_crash",
        ":runner_result",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/base:nullability",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "analyze_corpora_test",
    srcs = ["analyze_corpora_test.cc"],
    data = [
        "@com_google_fuzztest//centipede/testing:test_fuzz_target",
        "@com_google_fuzztest//centipede/testing:test_fuzz_target_trace_pc",
        "@com_google_fuzztest//centipede/testing:threaded_fuzz_target",
    ],
    deps = [
        ":analyze_corpora",
        ":binary_info",
        ":environment",
        ":symbol_table",
        ":test_coverage_util",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "shared_memory_blob_sequence_test",
    srcs = ["shared_memory_blob_sequence_test.cc"],
    deps = [
        ":shared_memory_blob_sequence",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "execution_metadata_test",
    srcs = ["execution_metadata_test.cc"],
    deps = [
        ":execution_metadata",
        ":shared_memory_blob_sequence",
        "@com_google_fuzztest//common:defs",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runner_result_test",
    srcs = ["runner_result_test.cc"],
    deps = [
        ":execution_metadata",
        ":feature",
        ":runner_result",
        ":shared_memory_blob_sequence",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "mutation_input_test",
    srcs = ["mutation_input_test.cc"],
    deps = [
        ":mutation_input",
        "@com_google_fuzztest//common:defs",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "byte_array_mutator_test",
    srcs = ["byte_array_mutator_test.cc"],
    deps = [
        ":byte_array_mutator",
        ":execution_metadata",
        ":knobs",
        ":mutation_input",
        ":runner_cmp_trace",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@com_google_fuzztest//common:defs",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "feature_test",
    srcs = ["feature_test.cc"],
    deps = [
        ":feature",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "foreach_nonzero_test",
    srcs = ["foreach_nonzero_test.cc"],
    deps = [
        ":foreach_nonzero",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "fuzztest_mutator_test",
    srcs = ["fuzztest_mutator_test.cc"],
    deps = [
        ":execution_metadata",
        ":fuzztest_mutator",
        ":knobs",
        ":mutation_input",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:defs",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "knobs_test",
    srcs = ["knobs_test.cc"],
    deps = [
        ":knobs",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/types:span",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "reverse_pc_table_test",
    srcs = ["reverse_pc_table_test.cc"],
    deps = [
        ":pc_info",
        ":reverse_pc_table",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "callstack_test",
    srcs = ["callstack_test.cc"],
    deps = [
        ":callstack",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@com_google_fuzztest//common:defs",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "feature_set_test",
    srcs = ["feature_set_test.cc"],
    deps = [
        ":feature",
        ":feature_set",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "corpus_io_test",
    srcs = ["corpus_io_test.cc"],
    deps = [
        ":corpus",
        ":corpus_io",
        ":feature",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/types:span",
        "@com_google_fuzztest//common:blob_file",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "corpus_test",
    srcs = ["corpus_test.cc"],
    deps = [
        ":binary_info",
        ":call_graph",
        ":control_flow",
        ":corpus",
        ":feature",
        ":feature_set",
        ":pc_info",
        ":util",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "command_test_helper",
    srcs = ["command_test_helper.cc"],
    deps = [
        ":runner_fork_server",
        "@abseil-cpp//absl/base:nullability",
    ],
)

cc_test(
    name = "command_test",
    srcs = ["command_test.cc"],
    data = [":command_test_helper"],
    deps = [
        ":command",
        ":stop",
        ":util",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runner_cmp_trace_test",
    srcs = ["runner_cmp_trace_test.cc"],
    deps = [
        ":runner_cmp_trace",
        "@abseil-cpp//absl/base:nullability",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "control_flow_test",
    srcs = ["control_flow_test.cc"],
    data = [
        "@com_google_fuzztest//centipede/testing:test_fuzz_target",
        "@com_google_fuzztest//centipede/testing:test_fuzz_target_trace_pc",
        "@com_google_fuzztest//centipede/testing:threaded_fuzz_target",
    ],
    deps = [
        ":binary_info",
        ":control_flow",
        ":pc_info",
        ":symbol_table",
        ":thread_pool",
        "@abseil-cpp//absl/container:flat_hash_map",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "call_graph_test",
    srcs = ["call_graph_test.cc"],
    deps = [
        ":call_graph",
        ":control_flow",
        ":pc_info",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@com_google_fuzztest//common:logging",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "runner_flags_test",
    srcs = ["runner_flags_test.cc"],
    deps = [
        ":runner_flags",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "seed_corpus_maker_lib_test",
    srcs = ["seed_corpus_maker_lib_test.cc"],
    deps = [
        ":feature",
        ":seed_corpus_maker_lib",
        ":workdir",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:remote_file",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "seed_corpus_maker_proto_lib_test",
    srcs = ["seed_corpus_maker_proto_lib_test.cc"],
    deps = [
        ":feature",
        ":seed_corpus_config_cc_proto",
        ":seed_corpus_maker_lib",
        ":seed_corpus_maker_proto_lib",
        ":workdir",
        "@abseil-cpp//absl/strings",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "coverage_test",
    srcs = ["coverage_test.cc"],
    data = [
        "@com_google_fuzztest//centipede/testing:test_fuzz_target",
        "@com_google_fuzztest//centipede/testing:test_fuzz_target_trace_pc",
        "@com_google_fuzztest//centipede/testing:threaded_fuzz_target",
    ],
    deps = [
        ":binary_info",
        ":control_flow",
        ":coverage",
        ":environment",
        ":feature",
        ":pc_info",
        ":symbol_table",
        ":test_coverage_util",
        ":thread_pool",
        ":util",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "resource_pool_test",
    srcs = ["resource_pool_test.cc"],
    deps = [
        ":resource_pool",
        ":rusage_stats",
        ":thread_pool",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:logging",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "periodic_action_test",
    srcs = ["periodic_action_test.cc"],
    deps = [
        ":periodic_action",
        "@abseil-cpp//absl/synchronization",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:logging",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "concurrent_bitset_test",
    srcs = ["concurrent_bitset_test.cc"],
    deps = [
        ":feature",
        ":thread_pool",
        "@abseil-cpp//absl/base:core_headers",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "concurrent_byteset_test",
    srcs = ["concurrent_byteset_test.cc"],
    deps = [
        ":feature",
        ":thread_pool",
        "@abseil-cpp//absl/base:core_headers",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "hashed_ring_buffer_test",
    srcs = ["hashed_ring_buffer_test.cc"],
    deps = [
        ":feature",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "int_utils_test",
    srcs = ["int_utils_test.cc"],
    deps = [
        ":int_utils",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "crash_summary_test",
    srcs = ["crash_summary_test.cc"],
    deps = [
        ":crash_summary",
        "@abseil-cpp//absl/log:check",
        "@googletest//:gtest_main",
    ],
)

################################################################################
#                               Other tests
################################################################################

cc_test(
    name = "centipede_test",
    timeout = "long",
    srcs = ["centipede_test.cc"],
    data = [
        "@com_google_fuzztest//centipede/testing:abort_fuzz_target",
        "@com_google_fuzztest//centipede/testing:expensive_startup_fuzz_target",
        "@com_google_fuzztest//centipede/testing:fuzz_target_with_config",
        "@com_google_fuzztest//centipede/testing:fuzz_target_with_custom_mutator",
        "@com_google_fuzztest//centipede/testing:hanging_fuzz_target",
        "@com_google_fuzztest//centipede/testing:seeded_fuzz_target",
        "@com_google_fuzztest//centipede/testing:test_fuzz_target",
        "@com_google_fuzztest//centipede/testing:test_input_filter",
    ],
    deps = [
        ":centipede_callbacks",
        ":centipede_default_callbacks",
        ":centipede_interface",
        ":environment",
        ":feature",
        ":mutation_input",
        ":runner_result",
        ":stop",
        ":util",
        ":workdir",
        "@abseil-cpp//absl/base:nullability",
        "@abseil-cpp//absl/container:flat_hash_set",
        "@abseil-cpp//absl/log",
        "@abseil-cpp//absl/log:check",
        "@abseil-cpp//absl/strings",
        "@abseil-cpp//absl/time",
        "@com_google_fuzztest//common:defs",
        "@com_google_fuzztest//common:hash",
        "@com_google_fuzztest//common:logging",
        "@com_google_fuzztest//common:test_util",
        "@googletest//:gtest_main",
    ],
)

# Verify that the `:centipede` target indeed creates a binary with the
# expected name.
sh_test(
    name = "centipede_binary_test",
    srcs = ["centipede_binary_test.sh"],
    data = [
        ":centipede",
        ":test_util_sh",
    ],
)
