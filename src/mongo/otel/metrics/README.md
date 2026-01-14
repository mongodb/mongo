# MongoDB Open Telemetry Metrics API

This module provides an OpenTelemetry-compatible metrics API for instrumenting MongoDB code.
Metrics are created through the `MetricsService` and can be tested using the provided test
utilities. For now, this is only supported in mongod, SERVER-116960 will add this in mongos.

## Creating Metrics

Metrics are created by calling the `create*` functions on the
[`MetricsService`](metrics_service.h), which is accessed via the `ServiceContext`:

```cpp
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_service.h"

void initializeMetrics(ServiceContext* svcCtx) {
    auto& metricsService = otel::metrics::MetricsService::get(svcCtx);

    auto* operationsCounter = metricsService.createInt64Counter(
        otel::metrics::MetricNames::kQueryCount,        // name
        "Number of queries executed",   // description
        otel::metrics::MetricUnit::kQueries);          // unit
}
```

Metrics should be stashed once they are created to avoid taking a lock on the global list of
metrics.

### MetricName Registry

All metric names must be registered in the [`MetricNames`](metric_names.h) class. This central
registry ensures the N&O team has full ownership over new OTel metrics in the server for
centralized collaboration with downstream OTel consumers. OTel metrics are stored in time-series
DBs by the SRE team, and a sudden increase in metrics will result in operational costs ballooning
for the SRE team, which is why N&O owns this registry.

When adding a new metric, add a `static constexpr MetricName` entry to the `MetricNames` class in
`metric_names.h`, grouped under your team name:

```cpp
class MetricNames {
public:
    // Query Team Metrics
    static constexpr MetricName kQueryCount = {"num_queries"};
    static constexpr MetricName kQueryLatency = {"query_latency"};
};
```

### Naming Conventions

Follow
[OpenTelemetry naming conventions](https://opentelemetry.io/docs/specs/semconv/general/naming/):

- Use lowercase with dots as separators for namespaces (e.g., `network.connections.active`), and
  underscores to separate words within namespaces (`slow_queries`)
- Put every metric within a namespace related to the context of the metric (e.g.,
  `network.connections.active`, rather than just `connections.active`)
- Be descriptive but concise, there is no need to restate the units as part of the metric name

`mongodb.` will be automatically prepended to all metric names because it is the service name
provided to OTel.

### Available Units

The [`MetricUnit`](metric_unit.h) enum provides standard units.

#### Adding New Units

If your metric requires a unit not listed above:

1. Add the new unit to the `MetricUnit` enum in [`metric_unit.h`](metric_unit.h)
2. Add the corresponding string conversion in [`metric_unit.cpp`](metric_unit.cpp)
3. Follow [OpenTelemetry semantic conventions for units][otel-units] where applicable

[otel-units]: https://opentelemetry.io/docs/specs/semconv/general/metrics/#instrument-units

Contact the N&O team if you're unsure whether to add a new unit or reuse an existing one.

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
auto* counter = otel::metrics::MetricsService::get(svcCtx).createInt64Counter(
    otel::metrics::MetricNames::kOperationsTotal,
    "Total number of operations performed",
    otel::metrics::MetricUnit::kOperations);

counter->add(1);  // Increment by 1
counter->add(10); // Increment by 10
```

**Important:** Counter values must only increase. Attempting to add a negative value will throw an
exception.

### Gauge

**Use when:** You need to track a value that can go up or down.

**Examples:**

- Current number of active connections
- Memory usage
- Queue depth
- Cache size

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

**Metric attributes are not currently supported.** The OpenTelemetry standard supports attaching
key-value attributes (also known as labels or tags) to metrics, enabling queries like
`query_count{database="admin"}`. However, this API intentionally does not expose attributes
because the default attribute implementation has performance implications. Please reach out to
the Networking and Observability team if you'd like this feature prioritized ([SERVER-117025](https://jira.mongodb.org/browse/SERVER-117025)).

## Performance Considerations

Understanding the performance characteristics of each metric type is critical for avoiding latency
regressions in hot code paths.

### Counters and Gauges: Lock-Free (Preferred for Hot Paths)

Counters and Gauges use **lock-free atomic operations** and are safe to use in
performance-sensitive code.

**Use Counters and Gauges** for metrics recorded on every request or in latency-critical paths.

### Histograms: Acquires Locks (Avoid in Hot Paths)

> [!WARNING]
> The underlying OpenTelemetry library acquires locks during histogram `Record()` operations.
> Avoid using histograms in performance-sensitive code paths where lock contention could impact
> latency or throughput. [SERVER-117030](https://jira.mongodb.org/browse/SERVER-117030) tracks
> improvements to histogram performance.

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
// GOOD: Create once, reuse the pointer
class MyClass {
    Counter<int64_t>* _counter;  // Stashed pointer

public:
    void init(ServiceContext* svc) {
        _counter = MetricsService::get(svc).createInt64Counter(...);
    }

    void doWork() {
        _counter->add(1);  // Fast: no lock
    }
};

// BAD: Creates/looks up metric on every call
void doWork(ServiceContext* svc) {
    auto* counter = MetricsService::get(svc).createInt64Counter(...);  // Slow: acquires lock
    counter->add(1);
}
```

## Testing Metrics

The [`metrics_test_util.h`](metrics_test_util.h) header provides utilities for testing that your
code correctly records metrics.

### OtelMetricsCapturer

`OtelMetricsCapturer` sets up an in-memory metrics exporter that captures all metrics created
during a test. **OtelMetricsCapturer must be constructed before any metrics are created** to
ensure they are captured.

```cpp
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {

TEST(MyFeatureTest, RecordsMetrics) {
    otel::metrics::OtelMetricsCapturer capturer;

    auto* counter = otel::metrics::MetricsService::get(getServiceContext()).createInt64Counter(
        otel::metrics::MetricNames::kMyFeatureEvents,
        "Number of events processed",
        otel::metrics::MetricUnit::kOperations);
    counter->add(5);

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
        "//src/mongo/otel/metrics:metrics_service",
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

## Exporting Metrics

Metrics can be exported in [OTLP format](https://opentelemetry.io/docs/specs/otlp/) using either
the file exporter or HTTP exporter. Configure these via server parameters at startup. Note that
only one exporter can be active at a time.

### File Exporter

Export metrics to local JSONL files by specifying a directory:

```bash
mongod --openTelemetryMetricsDirectory=/var/log/mongodb/metrics
```

Metrics are written to files with the pattern: `mongodb-{pid}-%Y%m%d-metrics.jsonl`

For example: `mongodb-12345-20251218-metrics.jsonl`

### HTTP Exporter

Export metrics to an OpenTelemetry collector or compatible backend via HTTP:

```bash
mongod --openTelemetryMetricsHttpEndpoint="http://localhost:4318/v1/metrics"
```

The HTTP exporter supports optional gzip compression:

```bash
mongod --openTelemetryMetricsHttpEndpoint="http://localhost:4318/v1/metrics" \
       --openTelemetryMetricsCompression=gzip
```

### Export Timing

Control how frequently metrics are exported and the timeout for export operations:

| Parameter                             | Description                       | Default |
| ------------------------------------- | --------------------------------- | ------- |
| `--openTelemetryExportIntervalMillis` | Time between consecutive exports  | 1000 ms |
| `--openTelemetryExportTimeoutMillis`  | Timeout for each export operation | 500 ms  |

### Additional Export Methods

Additional export methods (such as Prometheus Pull) are in development.

## Feature Flag

Metrics are gated behind the `featureFlagOtelMetrics` feature flag. The `OtelMetricsCapturer`
automatically enables this flag in tests. In production, ensure the flag is enabled for metrics
to be collected.
