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
    visibility = [
        "//tcmalloc:__subpackages__",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:percpu",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/time",
    ],
)

create_tcmalloc_testsuite(
    name = "tcmalloc_test",
    timeout = "long",
    srcs = ["tcmalloc_test.cc"],
    copts = REGTEST_OPTS,
    exec_properties = select({
        "//conditions:default": None,
    }),
    linkstatic = 1,  # get the most realistic performance
    shard_count = 3,
    tags = [
        "nosan",
    ],
    visibility = ["//visibility:private"],
    deps = REGTEST_DEPS + [
        "//tcmalloc/internal:config",
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

create_tcmalloc_testsuite(
    name = "memalign_test",
    srcs = ["memalign_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
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
create_tcmalloc_testsuite(
    name = "frag_test",
    srcs = ["frag_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
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
    tags = ["nosan"],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:linked_list",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "current_allocated_bytes_test",
    size = "small",
    srcs = ["current_allocated_bytes_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
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
    # Some experiments change sampling overhead.
    env = {"BORG_EXPERIMENTS": ""},
    linkstatic = 1,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random",
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
create_tcmalloc_testsuite(
    name = "sampler_test",
    timeout = "long",
    srcs = ["sampler_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        ":testutil",
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
    tags = ["nosan"],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:sysinfo",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "large_alloc_size_test",
    srcs = ["large_alloc_size_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        "//tcmalloc:malloc_extension",
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

create_tcmalloc_testsuite(
    name = "thread_ctor_test",
    srcs = ["thread_ctor_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":thread_ctor_test_lib",  # buildcleaner: keep
    ],
)

create_tcmalloc_testsuite(
    name = "sample_size_class_test",
    srcs = ["sample_size_class_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:linked_list",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "no_deps_test",
    srcs = ["no_deps_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/base:core_headers",
    ],
)

create_tcmalloc_testsuite(
    name = "no_deps_per_cpu_test",
    srcs = ["no_deps_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/base:core_headers",
    ],
)

create_tcmalloc_testsuite(
    name = "limit_test",
    srcs = ["limit_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    tags = ["nosan"],
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

create_tcmalloc_testsuite(
    name = "aligned_new_test",
    timeout = "long",
    srcs = ["aligned_new_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS + ["-faligned-new"] +
            select({
                "//conditions:default": [],
            }),
    linkstatic = 1,
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

create_tcmalloc_testsuite(
    name = "memory_errors_test",
    srcs = ["memory_errors_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = ["nosan"],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:stacktrace_filter",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/numeric:bits",
        "@com_google_absl//absl/strings",
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

create_tcmalloc_testsuite(
    name = "get_stats_test",
    srcs = ["get_stats_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/base:config",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:optional",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "releasing_test",
    srcs = ["releasing_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = ["nosan"],
    deps = [
        "//tcmalloc:malloc_extension",
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
    copts = ["-DTCMALLOC_INTERNAL_NUMA_AWARE"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc:tcmalloc_numa_aware",
    tags = [
    ],
    deps = [
        ":testutil",
        "//tcmalloc:common_numa_aware",
        "//tcmalloc:malloc_extension",
        "//tcmalloc:want_numa_aware",
        "//tcmalloc/internal:affinity",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:numa",
        "//tcmalloc/internal:page_size",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/base:no_destructor",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
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
    name = "background_test",
    srcs = ["background_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
        ":test_allocator_harness",
        ":testutil",
        ":thread_manager",
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest",
    ],
)

create_tcmalloc_testsuite(
    name = "reclaim_test",
    srcs = ["reclaim_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = ["nosan"],
    deps = [
        ":test_allocator_harness",
        ":thread_manager",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:parameter_accessors",
        "//tcmalloc/internal:percpu",
        "//tcmalloc/internal:sysinfo",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest",
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
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/container:flat_hash_map",
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

cc_test(
    name = "system-alloc_test",
    srcs = ["system-alloc_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = ["nosan"],
    deps = [
        "//tcmalloc:common_8k_pages",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:proc_maps",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "realloc_test",
    srcs = ["realloc_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = ["@com_google_googletest//:gtest_main"],
)

# This test has been named "large" since before tests were s/m/l.
# The "large" refers to large allocation sizes.
create_tcmalloc_testsuite(
    name = "tcmalloc_large_test",
    size = "small",
    timeout = "moderate",
    srcs = ["tcmalloc_large_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "noasan",
        "noubsan",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "malloc_extension_system_malloc_test",
    srcs = ["malloc_extension_system_malloc_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc/internal:system_malloc",
    deps = [
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "malloc_extension_test",
    srcs = ["malloc_extension_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
        ":testutil",
        "//tcmalloc:malloc_extension",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "malloc_tracing_extension_system_malloc_test",
    srcs = ["malloc_tracing_extension_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    local_defines = ["MALLOC_TRACING_EXTENSION_NOT_SUPPORTED"],
    malloc = "//tcmalloc/internal:system_malloc",
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:malloc_tracing_extension",
        "@com_google_absl//absl/cleanup",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "malloc_tracing_extension_test",
    srcs = ["malloc_tracing_extension_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:malloc_tracing_extension",
        "@com_google_absl//absl/cleanup",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "heap_profiling_test",
    srcs = ["heap_profiling_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
        "nosan",
    ],
    deps = [
        ":test_allocator_harness",
        ":thread_manager",
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:profile_builder",
        "//tcmalloc/internal:profile_cc_proto",
        "//tcmalloc/internal:sampled_allocation",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "peak_heap_profiling_test",
    srcs = ["peak_heap_profiling_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc/internal:parameter_accessors",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/log:check",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "variants_test",
    srcs = ["variants_test.cc"],
    deps = [
        "//tcmalloc:experiment",
        "//tcmalloc/internal:environment",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "parallel_test",
    srcs = ["parallel_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc/internal:system_malloc",
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc:tcmalloc_internal_methods_only",  # buildcleaner: keep
        "@com_google_absl//absl/log:absl_check",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "profile_drop_frames_test",
    srcs = ["profile_drop_frames_test.cc"],
    tags = [
        "noasan",
        "nomsan",
        "notsan",
        "noubsan",
    ],
    deps = [
        "//tcmalloc:malloc_extension",
        "//tcmalloc:new_extension",
        "//tcmalloc/internal:profile_builder",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "disable_numa_test",
    srcs = ["disable_numa_test.cc"],
    env = {"TCMALLOC_NUMA_AWARE": "0"},
    deps = [
        "//tcmalloc/internal:numa",
        "@com_google_googletest//:gtest_main",
    ],
)
