# MongoDB Open Telemetry Metrics API

This module provides an OpenTelemetry-compatible metrics API for instrumenting MongoDB code. Metrics
are created through the `MetricsService` and can be tested using the provided test utilities. This
is supported in mongos and mongod.

## Creating Metrics

Metrics are created by calling the `create*` functions on the [`MetricsService`](metrics_service.h),
which is accessed via the `MetricService` instance:

```cpp
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"

namespace {
auto& operationsCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kQueryCount,  // name
    "Number of queries executed",             // description
    otel::metrics::MetricUnit::kCount);       // unit
}  // namespace
```

Metrics should be stashed once they are created to avoid taking a lock on the global list of
metrics.

### MetricName Registry

All metric names must be registered in the [`MetricNames`](metric_names.h) class. This central
registry ensures the N&O team has full ownership over new OTel metrics in the server for centralized
collaboration with downstream OTel consumers. OTel metrics are stored in time-series DBs by the SRE
team, and a sudden increase in metrics will result in operational costs ballooning for the SRE team,
which is why N&O owns this registry.

When adding a new metric, add a `static constexpr MetricName` entry to the `MetricNames` class in
`metric_names.h`, grouped with related metrics:

```cpp
class MetricNames {
public:
    // Query Metrics
    static constexpr MetricName kQueryCount = MetricNameMaker::make("mongodb.query.count");
    static constexpr MetricName kQueryLatency = MetricNameMaker::make("mongodb.query.latency_millis");
};
```

When working on a module with metrics that should be internal-only, a module-specific "MetricNames"
class (e.g. `MyModuleMetricNames`) in a different file may be used, as long as N&O team has
ownership over the file defining the metric names. This additionally requires adding some code to
metric_names.h.

### Naming Conventions

#### Metric Names

All metrics, whether new or migrated from serverStatus, follow the
[OpenTelemetry naming conventions](https://opentelemetry.io/docs/specs/semconv/general/naming/).
Some notable rules are:

- **Lowercase, dot-separated namespaces** — use dots to separate path segments (e.g.,
  `mongodb.network.connections.active`) and `snake_case` within each segment (e.g., `slow_queries`).
- **Always prefer `snake_case` over `camelCase` or hyphens** — all path segments use `snake_case`
  (`totalTimeMicros` → `total_time_micros`, `block-manager` → `block_manager`, etc.).
- **No `_total` / `.total` suffix** — Avoid appending `total` to metric names since it may make the
  meaning of counters confusing.

Also, metric names should follow these MongoDB specific rules:

- **`mongodb.` prefix** — every metric name gets `mongodb.` at the beginning.
- **Drop redundant sub-prefixes** — e.g. the second `disk` in
  `mongodb.hardware.disk.disk_space_free_bytes.`
- **Provide sufficient context in the metric name for unitless metrics** - Unitless metrics should
  specify what the number means rather than adding a new unit for it. Such metrics should be created
  with a `count` unit. Example: we should choose a `docsInserted` metric with `count` unit over a
  `docs` metric with `numInserted` unit.

Note that not all metric names currently follow the above guidelines and are in the process of being
updated (TODO(SERVER-133643)). Any new metric should follow this guidance rather than the precedent
set by other metric names.

#### Units

The [`MetricUnit`](metric_unit.h) enum provides standard units.

Prefer generic unit names over domain-specific ones: the metric name already provides the context,
so a cursor count should use `MetricUnit::kCount` rather than a `kCursors`-style unit.

If your metric requires a unit not listed above:

1. Add the new unit to the `MetricUnit` enum in [`metric_unit.h`](metric_unit.h)
2. Add the corresponding string conversion in [`metric_unit.cpp`](metric_unit.cpp)
3. Follow [OpenTelemetry semantic conventions for units][otel-units] where applicable

[otel-units]: https://opentelemetry.io/docs/specs/semconv/general/metrics/#instrument-units

Contact the N&O team if you're unsure whether to add a new unit or reuse an existing one.

#### Migrating existing serverStatus metrics

Do **not** simply reuse the serverStatus name. Translate it into the spec above (e.g.
`serverStatus.metrics.network.totalTimeForEgressConnectionAcquiredToWireMicros` becomes
`mongodb.network.egress.connection_acquired_to_wire.time_micros`).

#### Avoid renaming released metrics

Once a metric has been released for external consumption and is documented, renaming it breaks
downstream consumers, dashboards, and alerts. Renames after release should be avoided.

## Metric Types

Choose the appropriate metric type based on what you're measuring:

### Counter

**Use when:** You need to track a value that only increases over time. Rate-based queries will
typically be run on these metrics.

**Examples:**

- Number of operations performed
- Total bytes transferred
- Number of connections established
- Query count

```cpp
auto& counter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kOperationsCount,
    "Total number of operations performed",
    otel::metrics::MetricUnit::kCount);

counter.add(1);  // Increment by 1
counter.add(10); // Increment by 10
```

**Important:** Counter values must only increase. Attempting to add a negative value will throw an
exception.

### UpDownCounter

**Use when:** You need a cumulative sum that can increase **and** decrease via `add()`
(OpenTelemetry
[UpDownCounter](https://opentelemetry.io/docs/specs/otel/metrics/api/#updowncounter)). This is
distinct from a **Gauge**, which represents an observed point-in-time value (see below).

**Examples:**

- Number of open ingress sessions (increment on accept, decrement on disconnect)
- In-flight requests or queue depth when updated synchronously with `add(±n)`

```cpp
auto& openSessions = otel::metrics::MetricsService::instance().createInt64UpDownCounter(
    otel::metrics::MetricNames::kOpenConnections,
    "Total number of open sessions",
    otel::metrics::MetricUnit::kCount);

openSessions.add(1);   // Session started
openSessions.add(-1);  // Session ended
```

### Gauge

**Use when:** You need to track a value that can go up or down **as an observed snapshot** (last
value), usually via the observable gauge callback rather than only synchronous `add()`.

**Examples:**

- Memory usage
- Queue depth
- Cache size

For a **running total** that goes up and down with explicit `add(1)` / `add(-1)` in application
code, prefer **UpDownCounter**.

### Histogram

**Use when:** You need to track the distribution of values.

**Examples:**

- Operation latencies
- Request sizes

#### Explicit Bucket Boundaries

Histograms can optionally be created with a list of explicit bucket boundaries. See the
documentation for `createInt64Histogram` in [`metrics_service.h`](metrics_service.h) for more
information.

## Metric Attributes (Labels)

The OpenTelemetry standard supports attaching key-value attributes (also known as labels or tags) to
metrics, so a single metric can be sliced by attribute value in the backend it is exported to. For
example, a `mongodb.query.count` counter with a `mongodb.database` attribute lets you break query
counts down per database.

**Metric attribute values are required to be known at metric creation time.** This reduces the
impact of attributes on performance, and helps prevent issues around PII in attributes, which is
currently disallowed. We may support non-compile-time attributes in the future, please reach out to
the Networking and Observability team if you'd like this feature prioritized
([SERVER-121629](https://jira.mongodb.org/browse/SERVER-121629)).

### Attribute Conventions

- Lowercase.
- Dot-separated namespacing with `snake_case` within each segment, following the same
  `{object}.{property}` pattern as metric names.
- MongoDB-specific attributes use the `mongodb.*` prefix.
- Singular for single values (`host.name`), plural for arrays (`process.command_args`).
- No ambiguous abbreviations.

## Performance Considerations

Understanding the performance characteristics of each metric type is critical for avoiding latency
regressions in hot code paths.

### Counters, UpDownCounters, and Gauges: Lock-Free (Preferred for Hot Paths)

Counters, UpDownCounters, and Gauges use **lock-free atomic operations** and are safe to use in
performance-sensitive code.

**Use Counters, UpDownCounters, and Gauges** for metrics recorded on every request or in
latency-critical paths.

### Histograms: Acquires Locks (Validate Overhead)

<!-- prettier-ignore -->
> [!WARNING]
> The underlying OpenTelemetry library acquires locks during histogram `Record()` operations. Ensure
> that histograms are only used when required and verify their overhead through performance testing. 
> [SERVER-117030](https://jira.mongodb.org/browse/SERVER-117030) tracks improvements to histogram
> performance.

**When to use histograms:**

- Sampling latencies (not on every operation)
- Background operations where latency is not critical
- Low-frequency events

**When NOT to use histograms:**

- Per-request latency recording on high-throughput paths
- Any code path where microsecond-level latency matters
- Hot loops or frequently-called functions

### Metric Creation: Acquires Lock (Cache the Result)

Creating metrics via `MetricsService::create*()` acquires a mutex. **Always stash metric pointers**
rather than calling `create*()` on each use:

```cpp
// GOOD: Create static metric once, reuse in the file.
namespace {
Counter<int64_t>& counter = MetricsService::instance().createInt64Counter(...);
}  // namespace

void doWork() {
    counter.add(1);
}
```

```cpp
// BAD: Creates/looks up metric on every call
void doWork() {
    auto& counter = MetricsService::instance().createInt64Counter(...);  // Slow: acquires lock
    counter.add(1);
}
```

## Testing Metrics

The [`metrics_test_util.h`](metrics_test_util.h) header provides utilities for testing that your
code correctly records metrics.

### OtelMetricsCapturer

`OtelMetricsCapturer` sets up an in-memory metrics exporter that captures all metrics created during
a test. **OtelMetricsCapturer must be constructed before any metrics are created** to ensure they
are captured.

```cpp
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {

TEST(MyFeatureTest, RecordsMetrics) {
    otel::metrics::OtelMetricsCapturer capturer;

    auto& counter = otel::metrics::MetricsService::instance().createInt64Counter(
        otel::metrics::MetricNames::kMyFeatureEvents,
        "Number of events processed",
        otel::metrics::MetricUnit::kCount);
    counter.add(5);

    // Some variants don't currently include otel (notably Windows and some suse variants) so
    // always condition on if metrics can be read if your tests will run in one of those variants.
    if (capturer.canReadMetrics()) {
        EXPECT_EQ(capturer.readInt64Counter(otel::metrics::MetricNames::kMyFeatureEvents), 5);
    }
}

}  // namespace mongo::otel::metrics
```

## Build Dependencies

To use the metrics API, add the appropriate dependency to your `BUILD.bazel`:

```python
mongo_cc_library(
    name = "my_library",
    # ...
    deps = [
        "//src/mongo/otel/metrics:otel_metrics_service",
    ],
)
```

For tests using the test utilities:

```python
mongo_cc_unit_test(
    name = "my_test",
    # ...
    deps = [
        "//src/mongo/otel/metrics:metrics_test_util",
    ],
)
```

## serverStatus Integration

Metrics can optionally be exposed in the `serverStatus` command response under the `metrics` section
by specifying `serverStatusOptions` when creating a metric:

```cpp
#include "mongo/db/topology/cluster_role.h"
#include "mongo/otel/metrics/metrics_service.h"

namespace {
auto& myCounter = otel::metrics::MetricsService::instance().createInt64Counter(
    otel::metrics::MetricNames::kMyFeatureEvents,
    "Number of events processed",
    otel::metrics::MetricUnit::kCount,
    {.serverStatusOptions = otel::metrics::ServerStatusOptions{.dottedPath = "myFeature.eventCount",
                                                               .role = ClusterRole::None}});
}  // namespace
```

This metric appears in the `serverStatus` response under the `metrics` section:

```json
{
  "metrics": {
    "myFeature": {
      "eventCount": NumberLong(42)
    }
  }
}
```

### Dotted Path

The `dottedPath` field specifies the path under `metrics`. Do **not** include a `metrics.` prefix.
It is added automatically.

### Role

The `role` field controls which node types expose the metric:

| Value                       | Exposed on          |
| --------------------------- | ------------------- |
| `ClusterRole::None`         | All node types      |
| `ClusterRole::ShardServer`  | Shard servers only  |
| `ClusterRole::RouterServer` | Router servers only |

### Serialization Format

Each metric type serializes differently in the serverStatus response:

| Metric Type   | serverStatus format                                        |
| ------------- | ---------------------------------------------------------- |
| Counter       | Scalar (`NumberLong` for `int64_t`, `double` for `double`) |
| UpDownCounter | Scalar (`NumberLong` for `int64_t`, `double` for `double`) |
| Gauge         | Scalar (`NumberLong` for `int64_t`, `double` for `double`) |
| Histogram     | `{ "average": <double>, "count": <NumberLong> }`           |

## Exporting Metrics

Metrics can be exported in [OTLP format](https://opentelemetry.io/docs/specs/otlp/) using either the
file exporter or HTTP exporter. Configure these via server parameters at startup. Note that only one
exporter can be active at a time.

### File Exporter

Export metrics to local JSONL files by specifying a directory:

```bash
mongod --setParameter openTelemetryMetricsDirectory=/var/log/mongodb/metrics
```

Metrics are written to files with the pattern: `mongodb-{pid}-%Y%m%d-%N-metrics.jsonl`, where `%N`
is the rotation index. Each file is rotated once it reaches ~8MB to keep individual files small
enough to be consumed by tooling (for example, the mongo shell's `cat()`), so the most recent
metrics may be spread across a small number of rotated files.

For example: `mongodb-12345-20251218-0-metrics.jsonl`

### HTTP Exporter

Export metrics to an OpenTelemetry collector or compatible backend via HTTP:

```bash
mongod --setParameter openTelemetryMetricsHttpEndpoint="http://localhost:4318/v1/metrics"
```

The HTTP exporter supports optional gzip compression:

```bash
mongod --setParameter openTelemetryMetricsHttpEndpoint="http://localhost:4318/v1/metrics" \
       --setParameter openTelemetryMetricsCompression=gzip
```

### Export Timing

Control how frequently metrics are exported and the timeout for export operations:

| Parameter                           | Description                       | Default |
| ----------------------------------- | --------------------------------- | ------- |
| `openTelemetryExportIntervalMillis` | Time between consecutive exports  | 1000 ms |
| `openTelemetryExportTimeoutMillis`  | Timeout for each export operation | 500 ms  |

### Additional Export Methods

Additional export methods (such as Prometheus Pull) are in development.

## Feature Flag

Metrics are gated behind the `featureFlagOtelMetrics` feature flag, which is enabled by default.
