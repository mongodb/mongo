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
#
# Description:
#
# tcmalloc is a fast malloc implementation.  See
# https://github.com/google/tcmalloc/tree/master/docs/design.md for a high-level description of
# how this malloc works.

load("@rules_fuzzing//fuzzing:cc_defs.bzl", "cc_fuzz_test")
load("//tcmalloc:copts.bzl", "TCMALLOC_DEFAULT_COPTS")
load("//tcmalloc:variants.bzl", "create_tcmalloc_benchmark", "create_tcmalloc_testsuite")

package(default_visibility = ["//visibility:private"])

licenses(["notice"])

exports_files(["LICENSE"])

config_setting(
    name = "llvm",
    flag_values = {
        "@bazel_tools//tools/cpp:compiler": "clang",
    },
    visibility = [
        "//tcmalloc/internal:__subpackages__",
        "//tcmalloc/testing:__subpackages__",
    ],
)

cc_library(
    name = "experiment",
    srcs = ["experiment.cc"],
    hdrs = [
        "experiment.h",
        "experiment_config.h",
    ],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//assistant/assistant_server/rpcserver/internal:__pkg__",
        "//tcmalloc/testing:__pkg__",
        "//util/compiler:__pkg__",
    ],
    deps = [
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:environment",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:optional",
    ],
)

# Dependencies required by :tcmalloc and its variants.  Since :common is built
# several different ways, it should not be included on this list.
tcmalloc_deps = [
    ":experiment",
    ":malloc_extension",
    ":malloc_tracing_extension",
    ":new_extension",
    ":sampled_allocation",
    "@com_google_absl//absl/base",
    "@com_google_absl//absl/base:config",
    "@com_google_absl//absl/base:core_headers",
    "@com_google_absl//absl/base:dynamic_annotations",
    "@com_google_absl//absl/debugging:leak_check",
    "@com_google_absl//absl/debugging:stacktrace",
    "@com_google_absl//absl/debugging:symbolize",
    "@com_google_absl//absl/memory",
    "@com_google_absl//absl/status",
    "@com_google_absl//absl/status:statusor",
    "@com_google_absl//absl/strings",
    "@com_google_absl//absl/numeric:bits",
    "//tcmalloc/internal:config",
    "//tcmalloc/internal:declarations",
    "//tcmalloc/internal:linked_list",
    "//tcmalloc/internal:logging",
    "//tcmalloc/internal:optimization",
    "//tcmalloc/internal:percpu",
]

# This library provides tcmalloc always
cc_library(
    name = "tcmalloc",
    srcs = [
        "libc_override.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common",
        "//tcmalloc/internal:overflow",
        "//tcmalloc/internal:page_size",
    ],
    alwayslink = 1,
)

# Provides tcmalloc always; use per-thread mode.
cc_library(
    name = "tcmalloc_deprecated_perthread",
    srcs = [
        "libc_override.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_DEPRECATED_PERTHREAD"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [
        "//tcmalloc/internal:__pkg__",
        "//tcmalloc/testing:__pkg__",
    ],
    deps = tcmalloc_deps + [
        ":common_deprecated_perthread",
        "//tcmalloc/internal:overflow",
        "//tcmalloc/internal:page_size",
    ],
    alwayslink = 1,
)

cc_library(
    name = "size_class_info",
    hdrs = ["size_class_info.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc/internal:config",
    ],
)

# List of common source files used by the various tcmalloc libraries.
common_srcs = [
    "allocation_sample.cc",
    "arena.cc",
    "arena.h",
    "background.cc",
    "central_freelist.cc",
    "central_freelist.h",
    "common.cc",
    "common.h",
    "cpu_cache.cc",
    "cpu_cache.h",
    "deallocation_profiler.cc",
    "experimental_pow2_size_class.cc",
    "global_stats.cc",
    "guarded_page_allocator.cc",
    "guarded_page_allocator.h",
    "hinted_tracker_lists.h",
    "huge_address_map.cc",
    "huge_allocator.cc",
    "huge_allocator.h",
    "huge_cache.cc",
    "huge_cache.h",
    "huge_page_aware_allocator.cc",
    "huge_page_aware_allocator.h",
    "huge_page_filler.h",
    "huge_pages.h",
    "huge_region.h",
    "legacy_size_classes.cc",
    "lifetime_based_allocator.cc",
    "lifetime_based_allocator.h",
    "page_allocator.cc",
    "page_allocator.h",
    "page_allocator_interface.cc",
    "page_allocator_interface.h",
    "page_heap.cc",
    "page_heap.h",
    "page_heap_allocator.h",
    "pagemap.cc",
    "pagemap.h",
    "parameters.cc",
    "peak_heap_tracker.cc",
    "sampler.cc",
    "sampler.h",
    "size_classes.cc",
    "sizemap.cc",
    "span.cc",
    "span.h",
    "span_stats.h",
    "stack_trace_table.cc",
    "stack_trace_table.h",
    "static_vars.cc",
    "static_vars.h",
    "stats.cc",
    "system-alloc.cc",
    "system-alloc.h",
    "thread_cache.cc",
    "thread_cache.h",
    "transfer_cache.cc",
    "transfer_cache.h",
    "transfer_cache_internals.h",
    "transfer_cache_stats.h",
]

common_hdrs = [
    "allocation_sample.h",
    "allocation_sampling.h",
    "arena.h",
    "central_freelist.h",
    "common.h",
    "cpu_cache.h",
    "deallocation_profiler.h",
    "global_stats.h",
    "guarded_page_allocator.h",
    "hinted_tracker_lists.h",
    "huge_address_map.h",
    "huge_allocator.h",
    "huge_cache.h",
    "huge_page_aware_allocator.h",
    "huge_page_filler.h",
    "huge_pages.h",
    "huge_region.h",
    "lifetime_based_allocator.h",
    "page_allocator.h",
    "page_allocator_interface.h",
    "page_heap.h",
    "page_heap_allocator.h",
    "pagemap.h",
    "pages.h",
    "parameters.h",
    "peak_heap_tracker.h",
    "sampled_allocation_allocator.h",
    "sampler.h",
    "sizemap.h",
    "span.h",
    "span_stats.h",
    "stack_trace_table.h",
    "static_vars.h",
    "stats.h",
    "system-alloc.h",
    "tcmalloc_policy.h",
    "thread_cache.h",
    "transfer_cache.h",
    "transfer_cache_internals.h",
    "transfer_cache_stats.h",
]

common_deps = [
    ":experiment",
    ":explicitly_constructed",
    ":malloc_extension",
    ":malloc_tracing_extension",
    ":new_extension",
    ":size_class_info",
    "@com_google_absl//absl/algorithm:container",
    "@com_google_absl//absl/base",
    "@com_google_absl//absl/base:config",
    "@com_google_absl//absl/base:core_headers",
    "@com_google_absl//absl/base:dynamic_annotations",
    "@com_google_absl//absl/container:fixed_array",
    "@com_google_absl//absl/container:flat_hash_map",  # For lifetime (deallocation) profiler
    "@com_google_absl//absl/debugging:stacktrace",
    "@com_google_absl//absl/debugging:symbolize",
    "@com_google_absl//absl/synchronization",
    "@com_google_absl//absl/hash:hash",
    "@com_google_absl//absl/memory",
    "@com_google_absl//absl/strings",
    "@com_google_absl//absl/strings:str_format",
    "@com_google_absl//absl/time",
    "@com_google_absl//absl/types:optional",
    "@com_google_absl//absl/types:span",
    "//tcmalloc/internal:atomic_stats_counter",
    "@com_google_absl//absl/numeric:bits",
    "//tcmalloc:sampled_allocation",
    "//tcmalloc:sampled_allocation_recorder",
    "//tcmalloc/internal:config",
    "//tcmalloc/internal:declarations",
    "//tcmalloc/internal:environment",
    "//tcmalloc/internal:lifetime_predictions",
    "//tcmalloc/internal:lifetime_tracker",
    "//tcmalloc/internal:linked_list",
    "//tcmalloc/internal:logging",
    "//tcmalloc/internal:memory_stats",
    "//tcmalloc/internal:mincore",
    "//tcmalloc/internal:numa",
    "//tcmalloc/internal:cache_topology",
    "//tcmalloc/internal:optimization",
    "//tcmalloc/internal:parameter_accessors",
    "//tcmalloc/internal:percpu",
    "//tcmalloc/internal:percpu_tcmalloc",
    "//tcmalloc/internal:prefetch",
    "//tcmalloc/internal:range_tracker",
    "//tcmalloc/internal:timeseries_tracker",
    "//tcmalloc/internal:util",
]

cc_library(
    name = "common",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [":tcmalloc_tests"],
    deps = common_deps + ["//tcmalloc/internal:page_size"],
    alwayslink = 1,
)

cc_library(
    name = "common_deprecated_perthread",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_DEPRECATED_PERTHREAD"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [":tcmalloc_tests"],
    deps = common_deps + ["//tcmalloc/internal:page_size"],
    alwayslink = 1,
)

cc_library(
    name = "explicitly_constructed",
    hdrs = ["explicitly_constructed.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = ["//tcmalloc:__subpackages__"],
    deps = [
        "//tcmalloc/internal:config",
    ],
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to use hugepage-aware
# allocator.
cc_library(
    name = "want_hpaa",
    srcs = ["want_hpaa.cc"],
    copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//tcmalloc/internal:config",
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to use hugepage-aware
# allocator with hpaa_subrelease=true.
cc_library(
    name = "want_hpaa_subrelease",
    srcs = ["want_hpaa_subrelease.cc"],
    copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,
    visibility = ["//visibility:public"],
    deps = [
        "//tcmalloc/internal:config",
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to not use hugepage-aware
# allocator.
cc_library(
    name = "want_no_hpaa",
    srcs = ["want_no_hpaa.cc"],
    copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc/testing:__pkg__",
    ],
    deps = [
        "//tcmalloc/internal:config",
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

# Add a dep to this if you want your binary to enable NUMA awareness by
# default.
cc_library(
    name = "want_numa_aware",
    srcs = ["want_numa_aware.cc"],
    copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//tcmalloc:__pkg__",
        "//tcmalloc/internal:__pkg__",
        "//tcmalloc/testing:__pkg__",
    ],
    deps = [
        "//tcmalloc/internal:config",
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

# TCMalloc with large pages is usually faster but fragmentation is higher.  See
# https://github.com/google/tcmalloc/tree/master/docs/tuning.md for more details.
cc_library(
    name = "tcmalloc_large_pages",
    srcs = [
        "libc_override.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_LARGE_PAGES"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common_large_pages",
        "//tcmalloc/internal:overflow",
        "//tcmalloc/internal:page_size",
    ],
    alwayslink = 1,
)

cc_library(
    name = "common_large_pages",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_LARGE_PAGES"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [":tcmalloc_tests"],
    deps = common_deps + ["//tcmalloc/internal:page_size"],
    alwayslink = 1,
)

# TCMalloc with 256k pages is usually faster but fragmentation is higher.  See
# https://github.com/google/tcmalloc/tree/master/docs/tuning.md for more details.
cc_library(
    name = "tcmalloc_256k_pages",
    srcs = [
        "libc_override.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_256K_PAGES"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common_256k_pages",
        "//tcmalloc/internal:overflow",
        "//tcmalloc/internal:page_size",
    ],
    alwayslink = 1,
)

cc_library(
    name = "common_256k_pages",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_256K_PAGES"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [":tcmalloc_tests"],
    deps = common_deps + ["//tcmalloc/internal:page_size"],
    alwayslink = 1,
)

cc_library(
    name = "tcmalloc_256k_pages_and_numa",
    srcs = [
        "libc_override.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = [
        "-DTCMALLOC_256K_PAGES",
        "-DTCMALLOC_NUMA_AWARE",
    ] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//tcmalloc/testing:__pkg__"],
    deps = tcmalloc_deps + [
        ":common_256k_pages_and_numa",
        "//tcmalloc/internal:overflow",
        "//tcmalloc/internal:page_size",
    ],
    alwayslink = 1,
)

cc_library(
    name = "common_256k_pages_and_numa",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = [
        "-DTCMALLOC_256K_PAGES",
        "-DTCMALLOC_NUMA_AWARE",
    ] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [":tcmalloc_tests"],
    deps = common_deps + ["//tcmalloc/internal:page_size"],
    alwayslink = 1,
)

# TCMalloc small-but-slow is a a version of TCMalloc that chooses to minimize
# fragmentation at a *severe* cost to performance.  It should be used by
# applications that have significant memory constraints, but don't need to
# frequently allocate/free objects.
#
# See https://github.com/google/tcmalloc/tree/master/docs/tuning.md for more details.
cc_library(
    name = "tcmalloc_small_but_slow",
    srcs = [
        "libc_override.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_SMALL_BUT_SLOW"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//visibility:public"],
    deps = tcmalloc_deps + [
        ":common_small_but_slow",
        "//tcmalloc/internal:overflow",
        "//tcmalloc/internal:page_size",
    ],
    alwayslink = 1,
)

cc_library(
    name = "common_small_but_slow",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_SMALL_BUT_SLOW"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [":tcmalloc_tests"],
    deps = common_deps + ["//tcmalloc/internal:page_size"],
    alwayslink = 1,
)

# TCMalloc with NUMA awareness compiled in. Note that by default NUMA awareness
# will still be disabled at runtime - this default can be changed by adding a
# dependency upon want_numa_aware, or overridden by setting the
# TCMALLOC_NUMA_AWARE environment variable.
cc_library(
    name = "tcmalloc_numa_aware",
    srcs = [
        "libc_override.h",
        "tcmalloc.cc",
        "tcmalloc.h",
    ],
    copts = ["-DTCMALLOC_NUMA_AWARE"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = ["//tcmalloc/testing:__pkg__"],
    deps = tcmalloc_deps + [
        ":common_numa_aware",
        "//tcmalloc/internal:overflow",
        "//tcmalloc/internal:page_size",
    ],
    alwayslink = 1,
)

cc_library(
    name = "common_numa_aware",
    srcs = common_srcs,
    hdrs = common_hdrs,
    copts = ["-DTCMALLOC_NUMA_AWARE"] + TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    visibility = [":tcmalloc_tests"],
    deps = common_deps + ["//tcmalloc/internal:page_size"],
    alwayslink = 1,
)

# Export some header files to //tcmalloc/testing/...
package_group(
    name = "tcmalloc_tests",
    packages = [
        "//tcmalloc/...",
    ],
)

cc_library(
    name = "mock_central_freelist",
    testonly = 1,
    srcs = ["mock_central_freelist.cc"],
    hdrs = ["mock_central_freelist.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "mock_static_forwarder",
    testonly = 1,
    hdrs = ["mock_static_forwarder.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        ":mock_transfer_cache",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "page_allocator_test_util",
    testonly = 1,
    srcs = [
        "page_allocator_test_util.h",
    ],
    hdrs = ["page_allocator_test_util.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [":tcmalloc_tests"],
    deps = [
        ":common",
        ":malloc_extension",
    ],
)

cc_test(
    name = "page_heap_test",
    srcs = ["page_heap_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/memory",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "mock_transfer_cache",
    testonly = 1,
    srcs = ["mock_transfer_cache.cc"],
    hdrs = ["mock_transfer_cache.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        ":mock_central_freelist",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:distributions",
        "@com_google_googletest//:gtest",
    ],
)

cc_fuzz_test(
    name = "huge_page_filler_fuzz",
    testonly = 1,
    srcs = ["huge_page_filler_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
    ],
    deps = [
        ":common",
        "//tcmalloc/internal:clock",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/log:check",
    ],
)

cc_fuzz_test(
    name = "span_fuzz",
    testonly = 1,
    srcs = ["span_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
    ],
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
    ],
)

cc_fuzz_test(
    name = "sizemap_fuzz",
    testonly = 1,
    srcs = ["sizemap_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
    ],
    deps = [
        ":common",
        ":size_class_info",
        "@com_google_absl//absl/log:check",
    ],
)

cc_fuzz_test(
    name = "transfer_cache_fuzz",
    testonly = 1,
    srcs = ["transfer_cache_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    tags = [
    ],
    deps = [
        ":common",
        ":mock_central_freelist",
        ":mock_transfer_cache",
    ],
)

cc_test(
    name = "arena_test",
    timeout = "moderate",
    srcs = ["arena_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "transfer_cache_test",
    timeout = "moderate",
    srcs = ["transfer_cache_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    shard_count = 3,
    deps = [
        ":common",
        ":mock_central_freelist",
        ":mock_transfer_cache",
        "//tcmalloc/internal:logging",
        "//tcmalloc/testing:thread_manager",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_benchmark(
    name = "transfer_cache_benchmark",
    srcs = ["transfer_cache_benchmark.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":mock_central_freelist",
        ":mock_transfer_cache",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_test(
    name = "huge_cache_test",
    srcs = ["huge_cache_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:clock",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_allocator_test",
    srcs = ["huge_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_page_filler_test",
    timeout = "long",
    srcs = ["huge_page_filler_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        ":common",
        "//tcmalloc/internal:clock",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_page_aware_allocator_test",
    srcs = ["huge_page_aware_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    tags = [
    ],
    deps = [
        ":common",
        ":malloc_extension",
        ":page_allocator_test_util",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:page_size",
        "//tcmalloc/testing:thread_manager",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/meta:type_traits",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_region_test",
    srcs = ["huge_region_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_benchmark(
    name = "guarded_page_allocator_benchmark",
    srcs = ["guarded_page_allocator_benchmark.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:page_size",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
    ],
)

cc_test(
    name = "guarded_page_allocator_test",
    srcs = ["guarded_page_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:page_size",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/numeric:bits",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "guarded_page_allocator_profile_test",
    srcs = ["guarded_page_allocator_profile_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "noasan",
        "nomsan",
        "notsan",
        "noubsan",
    ],
    deps = [
        ":common",
        ":malloc_extension",
        "//tcmalloc/testing:testutil",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "pagemap_test",
    srcs = ["pagemap_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:config",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "realloc_test",
    srcs = ["realloc_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = ["@com_google_googletest//:gtest_main"],
)

cc_test(
    name = "stack_trace_table_test",
    srcs = ["stack_trace_table_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "system-alloc_test",
    srcs = ["system-alloc_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        ":common",
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_googletest//:gtest_main",
    ],
)

# This test has been named "large" since before tests were s/m/l.
# The "large" refers to large allocation sizes.
cc_test(
    name = "tcmalloc_large_test",
    size = "small",
    timeout = "moderate",
    srcs = ["tcmalloc_large_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "noasan",
        "noubsan",
    ],
    deps = [
        ":common",
        ":malloc_extension",
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
        ":malloc_extension",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "malloc_extension_test",
    srcs = ["malloc_extension_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        ":common",
        ":malloc_extension",
        "//tcmalloc/internal:percpu",
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
        ":common",
        ":malloc_tracing_extension",
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
        ":malloc_tracing_extension",
        "@com_google_absl//absl/cleanup",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_fuzz_test(
    name = "malloc_extension_fuzz",
    testonly = 1,
    srcs = ["malloc_extension_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":malloc_extension",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_test(
    name = "lifetime_based_allocator_test",
    srcs = ["lifetime_based_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:clock",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:lifetime_predictions",
        "//tcmalloc/internal:lifetime_tracker",
        "//tcmalloc/internal:logging",
        "//tcmalloc/testing:testutil",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "page_allocator_test",
    srcs = ["page_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    deps = [
        ":common",
        ":malloc_extension",
        ":page_allocator_test_util",
        "//tcmalloc/internal:config",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "profile_test",
    size = "medium",
    timeout = "long",
    srcs = ["profile_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc",
    shard_count = 2,
    tags = [
        "noasan",
        "nomsan",
        "notsan",
    ],
    deps = [
        ":malloc_extension",
        ":new_extension",
        "//tcmalloc/internal:declarations",
        "//tcmalloc/internal:linked_list",
        "//tcmalloc/testing:testutil",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:optional",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "thread_cache_test",
    size = "medium",
    srcs = ["thread_cache_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    linkstatic = 1,
    malloc = "//tcmalloc:tcmalloc_deprecated_perthread",
    tags = [
        "nosan",
    ],
    deps = [
        ":malloc_extension",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:memory_stats",
        "//tcmalloc/internal:parameter_accessors",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "size_classes_test",
    srcs = ["size_classes_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":size_class_info",
        "//tcmalloc/internal:config",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "heap_profiling_test",
    srcs = ["heap_profiling_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    tags = [
        "nosan",
    ],
    deps = [
        ":common",
        ":malloc_extension",
        ":sampled_allocation",
        "//tcmalloc/internal:logging",
        "//tcmalloc/internal:profile_builder",
        "//tcmalloc/internal:profile_cc_proto",
        "//tcmalloc/testing:test_allocator_harness",
        "//tcmalloc/testing:thread_manager",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_set",
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
        ":common",
        ":malloc_extension",
        "//tcmalloc/internal:parameter_accessors",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/log:check",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "sampled_allocation",
    srcs = ["sampled_allocation.cc"],
    hdrs = ["sampled_allocation.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = ["//tcmalloc:__subpackages__"],
    deps = [
        ":sampled_allocation_recorder",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/debugging:stacktrace",
    ],
)

cc_test(
    name = "sampled_allocation_test",
    srcs = ["sampled_allocation_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":sampled_allocation",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/debugging:stacktrace",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "sampled_allocation_recorder",
    hdrs = ["sampled_allocation_recorder.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = ["//tcmalloc:__subpackages__"],
    deps = [
        "//tcmalloc/internal:config",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/functional:function_ref",
    ],
)

cc_test(
    name = "sampled_allocation_recorder_test",
    srcs = ["sampled_allocation_recorder_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":explicitly_constructed",
        ":sampled_allocation_recorder",
        "//tcmalloc/testing:thread_manager",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "span_test",
    srcs = ["span_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_benchmark(
    name = "span_benchmark",
    srcs = ["span_benchmark.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = ":tcmalloc",
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/random",
    ],
)

cc_test(
    name = "stats_test",
    srcs = ["stats_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "sampled_allocation_allocator_test",
    srcs = ["sampled_allocation_allocator_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "@com_google_absl//absl/debugging:stacktrace",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "huge_address_map_test",
    srcs = ["huge_address_map_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "malloc_extension",
    srcs = ["malloc_extension.cc"],
    hdrs = [
        "internal_malloc_extension.h",
        "malloc_extension.h",
    ],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [
        "//visibility:public",
    ],
    deps = [
        "//tcmalloc/internal:parameter_accessors",
        "@com_google_absl//absl/base:config",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/base:malloc_internal",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:optional",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "malloc_tracing_extension",
    srcs = ["malloc_tracing_extension.cc"],
    hdrs = [
        "internal_malloc_tracing_extension.h",
        "malloc_tracing_extension.h",
    ],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
    ],
)

cc_library(
    name = "new_extension",
    srcs = ["new_extension.cc"],
    hdrs = ["new_extension.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = [":__subpackages__"],
    deps = [
        ":malloc_extension",
        "@com_google_absl//absl/base:core_headers",
    ],
)

create_tcmalloc_testsuite(
    name = "new_extension_test",
    srcs = ["new_extension_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":malloc_extension",
        ":new_extension",
        "//tcmalloc/internal:page_size",
        "//tcmalloc/testing:testutil",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/numeric:bits",
        "@com_google_absl//absl/random",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "experiment_config_test",
    srcs = ["experiment_config_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":experiment",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_fuzz_test(
    name = "experiment_fuzz",
    testonly = 1,
    srcs = ["experiment_fuzz.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":experiment",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "cpu_cache_test",
    srcs = ["cpu_cache_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = ":tcmalloc",
    deps = [
        ":common",
        ":mock_transfer_cache",
        "//tcmalloc/internal:affinity",
        "//tcmalloc/internal:optimization",
        "//tcmalloc/testing:testutil",
        "//tcmalloc/testing:thread_manager",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "cpu_cache_activate_test",
    srcs = ["cpu_cache_activate_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = ":tcmalloc_deprecated_perthread",
    deps = [
        ":common_deprecated_perthread",
        ":malloc_extension",
        "//tcmalloc/internal:percpu",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "central_freelist_test",
    srcs = ["central_freelist_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        ":mock_static_forwarder",
        "//tcmalloc/internal:logging",
        "//tcmalloc/testing:thread_manager",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:fixed_array",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/numeric:bits",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_benchmark(
    name = "central_freelist_benchmark",
    srcs = ["central_freelist_benchmark.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        "@com_github_google_benchmark//:benchmark",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/types:optional",
    ],
)

cc_library(
    name = "profile_marshaler",
    srcs = ["profile_marshaler.cc"],
    hdrs = ["profile_marshaler.h"],
    copts = TCMALLOC_DEFAULT_COPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":malloc_extension",
        "//tcmalloc/internal:profile_builder",
        "@com_google_absl//absl/status:statusor",
        "@com_google_protobuf//:protobuf",
    ],
)

cc_test(
    name = "profile_marshaler_test",
    srcs = ["profile_marshaler_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":malloc_extension",
        ":profile_marshaler",
        "//tcmalloc/internal:fake_profile",
        "//tcmalloc/internal:profile_cc_proto",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
        "@com_google_protobuf//:protobuf",
    ],
)

# TEMPORARY. WILL BE REMOVED.
# Add a dep to this if you want your binary to use old size classes.
cc_library(
    name = "want_legacy_size_classes",
    srcs = ["want_legacy_size_classes.cc"],
    copts = ["-g0"] + TCMALLOC_DEFAULT_COPTS,
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/base:core_headers",
    ],
    alwayslink = 1,
)

cc_test(
    name = "want_legacy_size_classes_test",
    srcs = ["want_legacy_size_classes_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    malloc = "//tcmalloc",
    deps = [
        ":common",
        ":want_legacy_size_classes",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "allocation_sample_test",
    srcs = ["allocation_sample_test.cc"],
    copts = TCMALLOC_DEFAULT_COPTS,
    deps = [
        ":common",
        "//tcmalloc/internal:logging",
        "//tcmalloc/testing:thread_manager",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

create_tcmalloc_testsuite(
    name = "variants_test",
    srcs = ["variants_test.cc"],
    deps = [
        ":experiment",
        "//tcmalloc/internal:environment",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest_main",
    ],
)
