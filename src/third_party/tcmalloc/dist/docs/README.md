# TCMalloc

This repository contains the TCMalloc C++ code.

TCMalloc is Google's customized implementation of C's `malloc()` and C++'s
`operator new` used for memory allocation within our C and C++ code. TCMalloc is
a fast, multi-threaded malloc implementation.

## Building TCMalloc

[Bazel](https://bazel.build) is the official build system for TCMalloc.

The [TCMalloc Platforms Guide](platforms.md) contains information on platform
support for TCMalloc.

## Documentation

All users of TCMalloc should consult the following documentation resources:

*   The [TCMalloc Quickstart](quickstart.md) covers downloading, installing,
    building, and testing TCMalloc, including incorporating within your
    codebase.
*   The [TCMalloc Overview](overview.md) covers the basic architecture of
    TCMalloc, and how that may affect configuration choices.
*   The [TCMalloc Reference](reference.md) covers the C and C++ TCMalloc API
    endpoints.

More advanced usages of TCMalloc may find the following documentation useful:

*   The [TCMalloc Tuning Guide](tuning.md) covers the configuration choices in
    more depth, and also illustrates other ways to customize TCMalloc.
*   The [TCMalloc Design Doc](design.md) covers how TCMalloc works underneath
    the hood, and why certain design choices were made. Most developers will not
    need this level of implementation detail.
*   The [TCMalloc Compatibility Guide](compatibility.md) which documents our
    expectations for how our APIs are used.
*   The [history and differences](gperftools.md) between this repository and
    gperftools.

## Publications

We've published several papers relating to TCMalloc optimizations:

*   ["Beyond malloc efficiency to fleet efficiency: a hugepage-aware memory
    allocator" (OSDI 2021)](https://research.google/pubs/pub50370/) relating to
    the development and rollout of [Temeraire](temeraire.md), TCMalloc's
    hugepage-aware page heap implementation.
*   ["Adaptive Hugepage Subrelease for Non-moving Memory Allocators in
    Warehouse-Scale Computers" (ISMM
    2021)](https://research.google/pubs/pub50436/) relating to optimizations for
    releasing partial hugepages to the operating system.

## License

The TCMalloc library is licensed under the terms of the Apache license. See
LICENSE for more information.

Disclaimer: This is not an officially supported Google product.
