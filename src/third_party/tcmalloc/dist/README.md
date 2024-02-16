# TCMalloc

This repository contains the TCMalloc C++ code.

TCMalloc is Google's customized implementation of C's `malloc()` and C++'s
`operator new` used for memory allocation within our C and C++ code. TCMalloc is
a fast, multi-threaded malloc implementation.

## Building TCMalloc

[Bazel](https://bazel.build) is the official build system for TCMalloc.

The [TCMalloc Platforms Guide](docs/platforms.md) contains information on
platform support for TCMalloc.

## Documentation

All users of TCMalloc should consult the following documentation resources:

*   The [TCMalloc Quickstart](docs/quickstart.md) covers downloading,
    installing, building, and testing TCMalloc, including incorporating within
    your codebase.
*   The [TCMalloc Overview](docs/overview.md) covers the basic architecture of
    TCMalloc, and how that may affect configuration choices.
*   The [TCMalloc Reference](docs/reference.md) covers the C and C++ TCMalloc
    API endpoints.

More advanced usages of TCMalloc may find the following documentation useful:

*   The [TCMalloc Tuning Guide](docs/tuning.md) covers the configuration
    choices in more depth, and also illustrates other ways to customize
    TCMalloc. This also covers important operating system-level properties for
    improving TCMalloc performance.
*   The [TCMalloc Design Doc](docs/design.md) covers how TCMalloc works
    underneath the hood, and why certain design choices were made. Most
    developers will not need this level of implementation detail.
*   The [TCMalloc Compatibility Guide](docs/compatibility.md) which documents
    our expectations for how our APIs are used.

## License

The TCMalloc library is licensed under the terms of the Apache license. See
LICENSE for more information.

Disclaimer: This is not an officially supported Google product.
