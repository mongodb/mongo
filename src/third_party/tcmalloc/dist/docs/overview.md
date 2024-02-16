# TCMalloc Overview

TCMalloc is Google's customized implementation of C's `malloc()` and C++'s
`operator new` used for memory allocation within our C and C++ code. This custom
memory allocation framework is an alternative to the one provided by the C
standard library (on Linux usually through `glibc`) and C++ standard library.
TCMalloc is designed to be more efficient at scale than other implementations.

Specifically, TCMalloc provides the following benefits:

*   Performance scales with highly parallel applications.
*   Optimizations brought about with recent C++14 and C++17 standard
    enhancements, and by diverging slightly from the standard where performance
    benefits warrant. (These are noted within the
    [TCMalloc Reference](reference.md).)
*   Extensions to allow performance improvements under certain architectures,
    and additional behavior such as metric gathering.

## TCMalloc Cache Operation Mode

TCMalloc may operate in one of two fashions:

*   (default) per-CPU caching, where TCMalloc maintains memory caches local to
    individual logical cores. Per-CPU caching is enabled when running TCMalloc
    on any Linux kernel that utilizes restartable sequences (RSEQ). Support for
    RSEQ was merged in Linux 4.18.
*   per-thread caching, where TCMalloc maintains memory caches local to each
    application thread. If RSEQ is unavailable, TCMalloc reverts to using this
    legacy behavior.

NOTE: the "TC" in TCMalloc refers to Thread Caching, which was originally a
distinguishing feature of TCMalloc; the name remains as a legacy.

In both cases, these cache implementations allows TCMalloc to avoid requiring
locks for most memory allocations and deallocations.

## TCMalloc Features

TCMalloc provides APIs for dynamic memory allocation: `malloc()` using the C
API, and `::operator new` using the C++ API. TCMalloc, like most allocation
frameworks, manages this memory better than raw memory requests (such as through
`mmap()`) by providing several optimizations:

*   Performs allocations from the operating system by managing
    specifically-sized chunks of memory (called "pages"). Having all of these
    chunks of memory the same size allows TCMalloc to simplify bookkeeping.
*   Devoting separate pages (or runs of pages called "Spans" in TCMalloc) to
    specific object sizes. For example, all 16-byte objects are placed within a
    "Span" specifically allocated for objects of that size. Operations to get or
    release memory in such cases are much simpler.
*   Holding memory in *caches* to speed up access of commonly-used objects.
    Holding such caches even after deallocation also helps avoid costly system
    calls if such memory is later re-allocated.

The cache size can also affect performance. The larger the cache, the less any
given cache will overflow or get exhausted, and therefore require a lock to get
more memory. TCMalloc extensions allow you to modify this cache size, though the
default behavior should be preferred in most cases. For more information,
consult the [TCMalloc Tuning Guide](tuning.md).

Additionally, TCMalloc exposes telemetry about the state of the application's
heap via `MallocExtension`. This can be used for gathering profiles of the live
heap, as well as a snapshot taken near the heap's highwater mark size (a peak
heap profile).

## The TCMalloc API

TCMalloc implements the C and C++ dynamic memory API endpoints from the C11,
C++11, C++14, and C++17 standards.

From C++, this includes

*   The basic `::operator new`, `::operator delete`, and array variant
    functions.
*   C++14's sized `::operator delete`
*   C++17's overaligned `::operator new` and `::operator delete` functions.

Unlike in the standard implementations, TCMalloc does not throw an exception
when allocations fail, but instead crashes directly. Such behavior can be used
as a performance optimization for move constructors not currently marked
`noexcept`; such move operations can be allowed to fail directly due to
allocation failures. In [Abseil](https://abseil.io/docs/cpp/guides/base), these
are enabled with `-DABSL_ALLOCATOR_NOTHROW`.

From C, this includes `malloc`, `calloc`, `realloc`, and `free`.

The TCMalloc API obeys the behavior of C90 DR075 and
[DR445](http://www.open-std.org/jtc1/sc22/wg14/www/docs/summary.htm#dr_445)
which states:

> The alignment requirement still applies even if the size is too small for any
> object requiring the given alignment.

In other words, `malloc(1)` returns `alignof(std::max_align_t)`-aligned pointer.
Based on the progress of
[N2293](http://www.open-std.org/jtc1/sc22/wg14/www/docs/n2293.htm), we may relax
this alignment in the future.

For more complete information, consult the [TCMalloc Reference](reference.md).
