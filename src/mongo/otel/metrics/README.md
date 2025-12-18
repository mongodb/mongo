# MongoDB Open Telemetry Metrics API

This module provides an OpenTelemetry-compatible metrics API for instrumenting MongoDB code. Metrics are created through the `MetricsService` and can be tested using the provided test utilities.

## Creating Metrics

Metrics are created through the [`MetricsService`](https://github.com/mongodb/mongo/blob/a013280e0e5dc374f78adbc4cb68b4d190c1d9ed/src/mongo/otel/metrics/metrics_service.h), which is accessed via the `ServiceContext`:

```cpp
#include "mongo/otel/metrics/metrics_service.h"

void initializeMetrics(ServiceContext* svcCtx) {
    auto& metricsService = MetricsService::get(svcCtx);

    auto* operationsCounter = metricsService.createInt64Counter(
        "query.count",                  // name
        "Number of queries executed",   // description
        MetricUnit::kQueries);          // unit
}
```

### Naming Conventions

Follow [OpenTelemetry naming conventions](https://opentelemetry.io/docs/specs/semconv/general/naming/):

- Use lowercase with dots as separators for namespaces (e.g., `connections.active`), and underscores to separate words within namespaces (`slow_queries.count`)
- Be descriptive but concise

`mongodb.` will be automatically prepended to all metric names because it is the service name provided to OTel.

### Available Units

The [`MetricUnit` enum](https://github.com/mongodb/mongo/blob/a013280e0e5dc374f78adbc4cb68b4d190c1d9ed/src/mongo/otel/metrics/metric_units.h#L37) provides standard units. Please add any additional units to the enum as they are needed.

## Metric Types

Choose the appropriate metric type based on what you're measuring:

### Counter

**Use when:** You need to track a value that only increases over time. Rate-based queries will typically be run on these metrics.

**Examples:**

- Number of operations performed
- Total bytes transferred
- Number of connections established
- Query count

```cpp
auto* counter = MetricsService::get(svcCtx).createInt64Counter(
    "operations.total",
    "Total number of operations performed",
    MetricUnit::kOperations);

counter->add(1);  // Increment by 1
counter->add(10); // Increment by 10
```

**Important:** Counter values must only increase. Attempting to add a negative value will throw an exception.

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

## Testing Metrics

The [`metrics_test_util.h`](https://github.com/mongodb/mongo/blob/a013280e0e5dc374f78adbc4cb68b4d190c1d9ed/src/mongo/otel/metrics/metrics_test_util.h) header provides utilities for testing that your code correctly records metrics.

### OtelMetricsCapturer

The [`OtelMetricsCapturer`](https://github.com/mongodb/mongo/blob/a013280e0e5dc374f78adbc4cb68b4d190c1d9ed/src/mongo/otel/metrics/metrics_test_util.h#L96) sets up an in-memory metrics exporter that captures all metrics created during a test. **It must be constructed before creating any metrics** to ensure they are captured.

```cpp
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::metrics {

TEST(MyFeatureTest, RecordsMetrics) {
    OtelMetricsCapturer capturer;

    auto* counter = MetricsService::get(getServiceContext()).createInt64Counter(
        "myfeature.events",
        "Number of events processed",
        MetricUnit::kOperations);
    counter->add(5);

    ASSERT_EQ(metricsCapturer.readInt64Counter("myfeature.events"), 5);
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

Metrics can be exported in [OTLP format](https://github.com/open-telemetry/opentelemetry-proto/tree/main/docs) using either the file exporter or HTTP exporter. Configure these via server parameters at startup. Note that only one exporter can be active at a time.

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

Metrics are gated behind the `featureFlagOtelMetrics` feature flag. The `OtelMetricsCapturer` automatically enables this flag in tests. In production, ensure the flag is enabled for metrics to be collected.
