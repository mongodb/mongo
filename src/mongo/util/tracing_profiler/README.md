# Overview

Welcome to the lightweight tracing profiler tool.

This tool allows to instrument the mongod and export the stats via mongodb server status command.

The data that is collected includes:

- total time spent in the tagged scope
- count of how many times the scope was entered (or function called)
- approximate overhead of profiling for that scope

The profiler keeps track of nested scopes, in a similar way to a call stack.

Profiler uses CycleClock (src/third_party/abseil-cpp/dist/absl/base/internal/cycleclock.cc).
The time is measured in nanoseconds by converting the cycle counts to time by using cycle clock
frequency. The typical overhead per measurement is ~15-20ns, which makes it suitable for profiling
fast functions.

## Building

To use the profiler, enable the profilerStats while building:
bazel build --use-tracing-profiler=on ...

When using MONGO_PROFILER_SCOPE_XYZ macros without --use-tracing-profiler=on they will resolve to
no-op empty definitions.

# Requirements

Profiler tools assume that `mongosh` command line tool is installed and available in the path

## Instrumenting code

The simple way to instument a function:

```
#include "mongo/util/tracing_profiler/profiler.h"

void myFunction(...) {
    auto __spanGuard = MONGO_PROFILER_SPAN_ENTER("myNamespace::myFunction");
    ...
    do stuff
    ...
}
```

## Usage

Example usage:

```
# Query mongod profiler stats before and after 60sec, and write the scopes measurements to perf.json
buildscripts/tracing_profiler/profile_mongod.py --sleep 60 > perf.json

# Alternatively, query mongod profiler stats before and after waiting for CTRL-C signal, and write
# the scopes measurements to perf.json
buildscripts/tracing_profiler/profile_mongod.py > perf.json

# Print the measurements as TSV (i.e. for analysis in google sheets)
buildscripts/tracing_profiler/profile_format.py -i perf.json -f tsv

# Create a flame graph from the measurements
buildscripts/tracing_profiler/profile_format.py -i perf.json -f folded > perf.folded
~/FlameGraph/flamegraph.pl perf.folded > perf.svg

# Create a flame graph from the measurements, normalized to single call of myNamespace::myFunction1.
# The time spent will be normalized to time spent per single average occurence of
# myNamespace::myFunction1.
buildscripts/tracing_profiler/profile_format.py -i perf.json -f folded -n "myNamespace::myFunction1"> perf.folded
~/FlameGraph/flamegraph.pl perf.folded > perf.svg
```

## Profiler design

Profiler collects all metrics in a form of a call tree, where nodes in the tree represent the
measurement scopes. Nested scopes, that execution enters and leaves while other scope is active
are represented by child nodes. A path in the call tree is similar to a call stack.

Each participating threads maintains separate measurement, to avoid cache synchronization.
Each thread call tree is only allowed to be modified by that thread, and usually that tree remains
mostly immutable after it's warmed up.

This allows to mostly limit threat safety concerns, where
race conditions can only occur:

- when the thread is trying to modify call tree, while the other thread is trying to export stats.
  This is solved by the use of a shared_lock, as modifying the tree is a relatively rare event.
- when the thread is trying to update node metrics, while the other thread is trying to export stats.
  This is solved by use of atomic reads and writes to the appropriate counters.

When exporting stats, profiler takes snapshot of the metrics that each participating thread
accumulated and then aggregates them together. When thread terminates, it aggregates its final
metrics with the profiler state, that is also included in the aggregation.

Profiler collects and computes the following metrics:

- totalNanos - the total time that was spent in a given node (call path).
- netNanos - the total time, excluding the estimated time spent in the profiler code.
  This is computed by estimating the profiler overhead in all children nodes
  (that is fully observed by the parent node) and in the node itself (only half
  of the overhead time is observed by the node itself).
  Note: he estimated overhead may be inaccurate, and is based on the calibrated avg time
  needed to record a measurement, and also based on the estimate that the measurement
  itself also included around half of the overhead time of the act of measuring itself.
- exclusiveNanos - the time spent in the node excluding all children and profiler code.
- count - The number of time the scope was entered.
