# TCMalloc and gperftools

There are two projects on Github that are based on Google’s internal TCMalloc:
This repository and [gperftools](https://github.com/gperftools/gperftools). Both
are fast C/C++ memory allocators designed around a fast path that avoids
synchronizing with other threads for most allocations.

This repository is Google's current implementation of TCMalloc, used by ~all of
our C++ programs in production. The code is limited to the memory allocator
implementation itself.

## History

Google open-sourced its memory allocator as part of "Google Performance Tools"
in 2005. At the time, it became easy to externalize code, but more difficult to
keep it in-sync with our internal usage, as discussed by Titus Winters’ in
[his 2017 CppCon Talk](https://www.youtube.com/watch?v=tISy7EJQPzI) and the
"Software Engineering at Google" book. Subsequently, our internal implementation
diverged from the code externally. This project eventually was adopted by the
community as "gperftools."

## Differences

Since
[“Profiling a Warehouse-Scale Computer” (Kanev 2015)](https://research.google/pubs/pub44271/),
we have invested in improving application productivity via optimizations to the
implementation (per-CPU caches, sized delete, fast/slow path improvements,
[hugepage-aware backend](temeraire.md)).

Because this repository reflects our day-to-day usage, we've focused on the
platforms we regularly use and can see extensive testing and optimization.

This implementation is based on [Abseil](https://github.com/abseil/abseil-cpp).
Like Abseil, we do not attempt to provide ABI stability. Providing a stable ABI
could require compromising performance or adding otherwise unneeded complexity
to maintain stability. These caveats are noted in our
[Compatibility Guidelines](compatibility.md).

In addition to a memory allocator, the gperftools project contains a number of
other tools:

*   An All-Allocation Memory Profiler: We have found this prohibitively costly
    to use regularly, and instead focus on using low-overhead, always-on
    sampling profilers. This sampling based profiler is exposed in our
    `malloc_extension.h`.
*   A SIGPROF-based CPU Profiler: The Linux `perf` tool is decreasing our
    internal need for signal-based profiling. Additionally, with restartable
    sequences, signals interrupt the fastpath, leading to skew between the
    observed instruction pointer and where we actually spend CPU time.
*   A Heap Checker/Debug Allocator: The LeakSanitizer, AddressSanitizer, and
    MemorySanitizer suite provide higher accuracy and better performance.
*   A perl-based `pprof` tool: This project is now developed in Go and is
    [available on Github](https://github.com/google/pprof).

## Differences From Google's Implementation of TCMalloc

The configuration on Github mirrors our production defaults, with two notable
exceptions:

*   Many of our production servers start a background thread (via
    `tcmalloc::MallocExtension::ProcessBackgroundActions`) to regularly call
    `tcmalloc::MallocExtension::ReleaseMemoryToSystem`, while others never
    release memory in favor of better CPU performance. These tradeoffs are
    discussed in our [tuning page](tuning.md).
*   We do not activate [GWP ASan](gwp-asan.md) by default, but can be activated
    via `MallocExtension`.

Over time, we have found that configurability carries a maintenance burden.
While a knob can provide immediate flexibility, the increased complexity can
cause subtle problems for more rarely used combinations.
