# Copyright 2019 The TCMalloc Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# -*- mode: python; -*-

# Description:
# Tests for tcmalloc, including a performance test.

load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")
load("//tcmalloc:variants.bzl", "create_tcmalloc_benchmark_suite", "create_tcmalloc_testsuite")

licenses(["notice"])

REGTEST_DEPS = [
    ":test_allocator_harness",
    ":testutil",
    ":thread_manager",
    "@com_google_googletest//:gtest",
    "@com_google_absl//absl/base",
    "@com_google_absl//absl/base:core_headers",
    "@com_google_absl//absl/container:flat_hash_set",
    "@com_google_absl//absl/debugging:leak_check",
    "@com_google_absl//absl/random",
    "@com_google_absl//absl/random:distributions",
    "@com_google_absl//absl/strings",
    "@com_google_absl//absl/strings:str_format",
    "@com_google_absl//absl/synchronization",
    "@com_google_absl//absl/utility",
    "@com_google_absl//absl/numeric:bits",
    "//tcmalloc/internal:declarations",
    "//tcmalloc/internal:logging",
    "//tcmalloc/internal:parameter_accessors",
    "//tcmalloc:malloc_extension",
    "//tcmalloc:new_extension",
]

REGTEST_OPTS = [
    "$(STACK_FRAME_UNLIMITED)",  # required for tcmalloc_test.cc
] + TCMALLOC_DEFAULT_COPTS

cc_library(
    name = "testutil",
    testonly = 1,
    srcs = ["testutil.cc"],
    hdrs = ["testutil.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = ["//tcmalloc:__subpackages__"],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:percpu",
        "@com_github_google_benchmark//:benchmark",
    ],
)

create_tcmalloc_testsuite(
    name = "tcmalloc_test",
    timeout = "long",
    srcs = ["tcmalloc_test.cc"],
    copts = REGTEST_OPTS,
    linkstatic = 1,  # get the most realistic performance
    tags = [
        "nosan",
    ],
    visibility = ["//visibility:private"],
    deps = REGTEST_DEPS + [
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:optional",
    ],
)

create_tcmalloc_benchmark_suite(
    name = "tcmalloc_benchmark",
    srcs = ["tcmalloc_benchmark.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//visibility:private",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:declarations",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "tcmalloc_test_deprecated_perthread",
    timeout = "long",
    srcs = ["tcmalloc_test.cc"],
    copts = REGTEST_OPTS + ["-DTCMALLOC_DEPRECATED_PERTHREAD"],
    linkstatic = 1,  # get the most realistic performance
    malloc = "//tcmalloc:tcmalloc_deprecated_perthread",
    tags = [
        "nosan",
    ],
    deps = REGTEST_DEPS + [
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:optional",
        "//tcmalloc:common_deprecated_perthread",
    ],
)

create_tcmalloc_testsuite(
    name = "threadcachesize_test",
    srcs = ["threadcachesize_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/synchronization",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "markidle_test",
    srcs = ["markidle_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_deprecated_perthread",
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/types:optional",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "memalign_test",
    srcs = ["memalign_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc/internal:page_size",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

# Fragmentation timing and test
cc_test(
    name = "frag_test",
    srcs = ["frag_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:common",
        "//tcmalloc:malloc_extension",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "largesmall_frag_test",
    srcs = ["largesmall_frag_test.cc"],
    copts = [
        "$(STACK_FRAME_UNLIMITED)",  # largesmall_frag_test.cc
    ] + TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:linked_list",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "thread_manager",
    testonly = 1,
    hdrs = ["thread_manager.h"],
    visibility = ["//tcmalloc:tcmalloc_tests"],
    deps = [
        "@com_google_absl//absl/synchronization",
        "@com_google_googletest//:gtest",
    ],
)

cc_test(
    name = "outofmemory_test",
    srcs = ["outofmemory_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:linked_list",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "current_allocated_bytes_test",
    size = "small",
    srcs = ["current_allocated_bytes_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
    ],
)

cc_test(
    name = "sampling_test",
    timeout = "long",
    srcs = ["sampling_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        ":thread_manager",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:optional",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "sampling_memusage_test",
    timeout = "long",
    srcs = ["sampling_memusage_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:affinity",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:optional",
        "@com_google_googletest//:gtest",
    ],
)

# Test of sampler class
cc_test(
    name = "sampler_test",
    timeout = "long",
    srcs = ["sampler_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    deps = [
        ":testutil",
        "//tcmalloc:common",
        "//tcmalloc:malloc_extension",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest_main",
    ],
)

# Check amount of memory allocated at startup.
cc_test(
    name = "startup_size_test",
    srcs = ["startup_size_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "large_alloc_size_test",
    srcs = ["large_alloc_size_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    deps = [
        "//tcmalloc:common",
        "//tcmalloc:malloc_extension",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "lifetimes_test",
    srcs = ["lifetimes_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    deps = [
        ":testutil",
        "//tcmalloc:common",
        "//tcmalloc/internal:lifetime_tracker",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

# Test for b/3485510.  Threads which heavily use malloc before TCMalloc's
# static initializers are invoked caused a race and crashes before cl/19792144
# fixed that race.
cc_library(
    name = "thread_ctor_test_lib",
    testonly = 1,
    srcs = ["thread_ctor_test_lib.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkopts = ["-pthread"],
)

cc_test(
    name = "thread_ctor_test",
    srcs = ["thread_ctor_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [":thread_ctor_test_lib"],
)

cc_test(
    name = "sample_size_class_test",
    srcs = ["sample_size_class_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:linked_list",
        "//tcmalloc/internal:logging",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "no_deps_test",
    srcs = ["no_deps_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = ["//tcmalloc:malloc_extension"],
)

create_tcmalloc_testsuite(
    name = "no_deps_per_cpu_test",
    srcs = ["no_deps_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    env = {"FORCE_CPU_CACHES": "1"},
    linkstatic = 1,
    deps = ["//tcmalloc:malloc_extension"],
)

create_tcmalloc_testsuite(
    name = "limit_test",
    srcs = ["limit_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:parameter_accessors",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:inlined_vector",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "aligned_new_test",
    timeout = "long",
    srcs = ["aligned_new_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS + ["-faligned-new"] +
            select({
                "//conditions:default": [],
            }),
    linkstatic = 1,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/debugging:leak_check",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "memory_errors_test",
    srcs = ["memory_errors_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "nompu64",  # until b/243344691 is fixed
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:common",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:declarations",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/numeric:bits",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_32k",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_large_pages",
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_32k_no_hpaa",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_large_pages",
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc:want_no_hpaa",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_256k",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_256k_pages",
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_256k_no_hpaa",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_256k_pages",
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc:want_no_hpaa",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_hpaa",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc:want_hpaa",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_subrelease",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc:want_hpaa_subrelease",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_no_hpaa",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc:want_no_hpaa",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_small",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_small_but_slow",
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/strings",
    ],
)

cc_binary(
    name = "want_hpaa_test_helper_small_no_hpaa",
    testonly = 1,
    srcs = ["want_hpaa_test_helper.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_small_but_slow",
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc:want_no_hpaa",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "get_stats_test",
    srcs = ["get_stats_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:common",
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:optional",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "releasing_test",
    srcs = ["releasing_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = ["nosan"],
    deps = [
        "//tcmalloc:common",
        "//tcmalloc:malloc_extension",
        "//tcmalloc:want_hpaa",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:memory_stats",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/types:optional",
        "@com_google_googletest//:gtest",
    ],
)

create_tcmalloc_testsuite(
    name = "default_parameters_test",
    srcs = ["default_parameters_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
    ],
)

cc_binary(
    name = "hello_main",
    testonly = 1,
    srcs = ["hello_main.cc"],
    malloc = "//tcmalloc",
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_library(
    name = "benchmark_main",
    srcs = ["benchmark_main.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = ["@com_github_google_benchmark//:benchmark"],
)

cc_test(
    name = "numa_locality_test",
    srcs = ["numa_locality_test.cc"],
    copts = ["-DTCMALLOC_NUMA_AWARE"] + TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc:tcmalloc_numa_aware",
    tags = ["requires-net:external"],
    deps = [
        ":testutil",
        "//tcmalloc:common_numa_aware",
        "//tcmalloc:want_numa_aware",
        "//tcmalloc/internal:affinity",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:numa",
        "//tcmalloc/internal:page_size",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "test_allocator_harness",
    testonly = 1,
    hdrs = ["test_allocator_harness.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        ":testutil",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/synchronization",
        "@com_google_googletest//:gtest",
    ],
)

create_tcmalloc_testsuite(
    name = "realized_fragmentation_test",
    timeout = "long",
    srcs = ["realized_fragmentation_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:linked_list",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "reclaim_test",
    srcs = ["reclaim_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
        ":test_allocator_harness",
        ":thread_manager",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:parameter_accessors",
        "//tcmalloc/internal:percpu",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "profile_test",
    srcs = ["profile_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "//tcmalloc:profile_marshaler",
        "//tcmalloc/internal:profile_builder",
        "//tcmalloc/internal:profile_cc_proto",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
        "@com_google_protobuf//:protobuf",
    ],
)

create_tcmalloc_testsuite(
    name = "deallocation_profiler_test",
    srcs = ["deallocation_profiler_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = ["nosan"],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)
