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

""" Helper functions to simplify TCMalloc BUILD files """

variants = [
    {
        "name": "8k_pages",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common"],
        "copts": [],
    },
    {
        "name": "32k_pages",
        "malloc": "//tcmalloc:tcmalloc_large_pages",
        "deps": ["//tcmalloc:common_large_pages"],
        "copts": ["-DTCMALLOC_LARGE_PAGES"],
    },
    {
        "name": "256k_pages",
        "malloc": "//tcmalloc:tcmalloc_256k_pages",
        "deps": ["//tcmalloc:common_256k_pages"],
        "copts": [
            "-DTCMALLOC_256K_PAGES",
        ],
    },
    {
        "name": "256k_pages_and_numa_disabled",
        "malloc": "//tcmalloc:tcmalloc_256k_pages_and_numa",
        "deps": ["//tcmalloc:common_256k_pages_and_numa"],
        "copts": [
            "-DTCMALLOC_256K_PAGES",
            "-DTCMALLOC_NUMA_AWARE",
        ],
    },
    {
        "name": "256k_pages_and_numa_enabled",
        "malloc": "//tcmalloc:tcmalloc_256k_pages_and_numa",
        "deps": ["//tcmalloc:common_256k_pages_and_numa"],
        "copts": [
            "-DTCMALLOC_256K_PAGES",
            "-DTCMALLOC_NUMA_AWARE",
        ],
        "env": {"TCMALLOC_NUMA_AWARE": "1"},
    },
    {
        "name": "small_but_slow",
        "malloc": "//tcmalloc:tcmalloc_small_but_slow",
        "deps": ["//tcmalloc:common_small_but_slow"],
        "copts": ["-DTCMALLOC_SMALL_BUT_SLOW"],
    },
    {
        "name": "256k_pages_pow2",
        "malloc": "//tcmalloc:tcmalloc_256k_pages",
        "deps": [
            "//tcmalloc:common_256k_pages",
        ],
        "copts": ["-DTCMALLOC_256K_PAGES"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_POW2_SIZECLASS"},
    },
    {
        "name": "256k_pages_sharded_transfer_cache",
        "malloc": "//tcmalloc:tcmalloc_256k_pages",
        "deps": [
            "//tcmalloc:common_256k_pages",
        ],
        "copts": ["-DTCMALLOC_256K_PAGES"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE"},
    },
    {
        "name": "numa_aware",
        "malloc": "//tcmalloc:tcmalloc_numa_aware",
        "deps": [
            "//tcmalloc:common_numa_aware",
            "//tcmalloc:want_numa_aware",
        ],
        "copts": ["-DTCMALLOC_NUMA_AWARE"],
    },
    {
        "name": "256k_pages_pow2_sharded_transfer_cache",
        "malloc": "//tcmalloc:tcmalloc_256k_pages",
        "deps": [
            "//tcmalloc:common_256k_pages",
        ],
        "copts": ["-DTCMALLOC_256K_PAGES"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_POW2_SIZECLASS,TEST_ONLY_TCMALLOC_SHARDED_TRANSFER_CACHE"},
    },
    {
        "name": "legacy_size_classes",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common",
            "//tcmalloc:want_legacy_size_classes",
        ],
        "copts": [],
    },
    {
        "name": "use_huge_region_more_often",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_USE_HUGE_REGIONS_MORE_OFTEN"},
    },
    {
        "name": "separate_allocs_for_few_and_many_objects_spans",
        "malloc": "//tcmalloc",
        "deps": ["//tcmalloc:common"],
        "env": {"BORG_EXPERIMENTS": "TEST_ONLY_TCMALLOC_SEPARATE_ALLOCS_FOR_FEW_AND_MANY_OBJECTS_SPANS"},
    },
    {
        "name": "no_hpaa",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common",
            "//tcmalloc:want_no_hpaa",
        ],
    },
    {
        "name": "hpaa",
        "malloc": "//tcmalloc",
        "deps": [
            "//tcmalloc:common",
            "//tcmalloc:want_hpaa",
        ],
    },
]

def create_tcmalloc_variant_targets(create_one, name, srcs, **kwargs):
    """ Invokes create_one once per TCMalloc variant

    Args:
      create_one: A function invoked once per variant with arguments
        matching those of a cc_binary or cc_test target.
      name: The base name, suffixed with variant names to form target names.
      srcs: Source files to be built.
      **kwargs: Other arguments passed through to create_one.

    Returns:
      A list of the targets generated; i.e. each name passed to create_one.
    """
    copts = kwargs.pop("copts", [])
    deps = kwargs.pop("deps", [])
    linkopts = kwargs.pop("linkopts", [])

    variant_targets = []
    for variant in variants:
        inner_target_name = name + "_" + variant["name"]
        variant_targets.append(inner_target_name)
        env = kwargs.pop("env", {})
        env.update(variant.get("env", {}))
        create_one(
            inner_target_name,
            copts = copts + variant.get("copts", []),
            linkopts = linkopts + variant.get("linkopts", []),
            malloc = variant.get("malloc"),
            srcs = srcs,
            deps = deps + variant.get("deps", []),
            env = env,
            **kwargs
        )

    return variant_targets

# Declare an individual test.
def create_tcmalloc_test(
        name,
        copts,
        linkopts,
        malloc,
        srcs,
        deps,
        **kwargs):
    native.cc_test(
        name = name,
        srcs = srcs,
        copts = copts,
        linkopts = linkopts,
        malloc = malloc,
        deps = deps,
        **kwargs
    )

# Create test_suite of name containing tests variants.
def create_tcmalloc_testsuite(name, srcs, **kwargs):
    variant_targets = create_tcmalloc_variant_targets(
        create_tcmalloc_test,
        name,
        srcs,
        **kwargs
    )
    native.test_suite(name = name, tests = variant_targets)

# Declare a single benchmark binary.
def create_tcmalloc_benchmark(name, srcs, **kwargs):
    deps = kwargs.pop("deps")
    malloc = kwargs.pop("malloc", "//tcmalloc")

    native.cc_binary(
        name = name,
        srcs = srcs,
        malloc = malloc,
        testonly = 1,
        linkstatic = 1,
        deps = deps + ["//tcmalloc/testing:benchmark_main"],
        **kwargs
    )

# Declare a suite of benchmark binaries, one per variant.
def create_tcmalloc_benchmark_suite(name, srcs, **kwargs):
    variant_targets = create_tcmalloc_variant_targets(
        create_tcmalloc_benchmark,
        name,
        srcs,
        **kwargs
    )

    # The first 'variant' is the default 8k_pages configuration. We alias the
    # benchmark name without a suffix to that target so that the default
    # configuration can be invoked without a variant suffix.
    native.alias(
        name = name,
        actual = variant_targets[0],
    )
