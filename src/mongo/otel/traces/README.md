# MongoDB OpenTelemetry Traces API

This module provides an OpenTelemetry-compatible tracing API for instrumenting MongoDB code. Spans
are created through the [`Span`](span/span.h) class and can be tested using the provided test
utilities. Tracing is supported in both mongod and mongos.

A **trace** is a tree of **spans** representing the path of an operation through the server. Each
span records a named, timed unit of work, along with optional attributes and a status. Parent-child
relationships between spans are propagated through a [`TelemetryContext`](../telemetry_context.h),
which can also be serialized across the wire so that a trace spans multiple nodes.

Whether a given span is actually part of an exported trace is decided by a combination of whether
its parent is already in a trace and the sampling decision made by the [`TracingSampler`](#sampling)
(see below). When a span is not sampled, the `Span` object is a cheap no-op.

## Creating Spans

Spans are RAII objects: they begin when created via one of the `Span::start` factory functions and
end automatically when the `Span` goes out of scope. Do **not** construct a `Span` directly — always
use a `start` function so parent-child relationships are established correctly.

When an `OperationContext` is available, prefer the `opCtx` overload. It fetches and stores the
current `TelemetryContext` on the `OperationContext` for you, so calling code does not have to
manage its own context:

```cpp
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"

void doWork(OperationContext* opCtx) {
    auto span = otel::traces::Span::start(opCtx, otel::traces::span_names::kMySpan);
    TRACING_SPAN_ATTR(span, "myKey", "myValue");

    // ... do the work; the span ends when it goes out of scope.
}
```

When no `OperationContext` is available, or when you need to create concurrent spans from a single
parent (see [Cloning a TelemetryContext](#cloning-a-telemetrycontext) below), pass a
`std::shared_ptr<TelemetryContext>` explicitly. If the pointer is empty but a context will be needed
going forward, `start` populates it with a newly created one so subsequent spans can be parented
correctly:

```cpp
auto span = otel::traces::Span::start(telemetryCtx, otel::traces::span_names::kMySpan);
TRACING_SPAN_ATTR(span, "reshardingUUID", uuid.toString());
```

### Cloning a TelemetryContext

When multiple spans will be started concurrently from the same parent, clone the `TelemetryContext`
so each new span is tied to the correct parent span rather than to each other. See the documentation
in [`telemetry_context.h`](../telemetry_context.h) for details.

## Span Names

All span names must be registered in the [`span_names`](span/span_names.h) namespace. This central
registry helps the N&O team see how spans are being used, enforce conventions, and develop
optimizations.

When adding a new span, add a `SPAN_NAME_` entry to `span_names.h`, grouped under your team name:

```cpp
// My Team Spans
SPAN_NAME_(kMySpan, "my_feature.do_work");
```

It is atypical but possible for span names to be registered dynamically (e.g., for command names).
Consult the N&O team if you think you have a new case for dynamically-registered span names.

### Naming Conventions

Follow
[OpenTelemetry naming conventions](https://opentelemetry.io/docs/specs/semconv/general/naming/):

- Use lowercase with dots as separators for namespaces, and underscores to separate words within a
  namespace (e.g. `resharding_coordinator.run_until_ready_to_commit`).
- Put every span within a namespace related to the context of the span.
- Be descriptive but concise.

## Span Attributes

Add attributes to a span with the `TRACING_SPAN_ATTR(span, key, value)` macro. Both integer and
string values are supported. Always use the macro rather than calling `Span::setAttribute` directly
so that the call compiles away to nothing when OTel is disabled at compile time.

<!-- prettier-ignore -->
> [!WARNING]
> Span attributes MUST NOT contain PII.

## Span Status

Set a span's status with `Span::setStatus(const Status&)`. If the status code is non-zero, the
associated OpenTelemetry span is marked as an error, and the error message is added as an attribute.

## Sampling

Head-based sampling decides which spans initiate a trace. Sampling is configured at runtime via
server parameters (see [Configuration](#configuration)):

- **Default-sampled spans** — spans registered as default-sampled are eligible for the default
  sampling strategy. Typically only the outermost entry points of operations we want traces for
  should be registered this way; child spans within a sampled trace are captured automatically.
- **Per-span overrides** — the sampling factor and rate limit can be overridden per span name.

Each sampling strategy combines a **sampling factor** (fraction of spans to sample, in `[0, 1]`)
with a **token-bucket rate limit** (refill rate and max tokens) to cap the absolute rate of sampled
traces.

Head-based sampling is gated behind the `featureFlagOtelTraceSampling` feature flag (disabled by
default).

### Externally-initiated Traces (Ingress Spans)

When an incoming command contains a trace context, the server will continue the trace, subject to a
configurable rate limit. When the incoming command does not contain a trace context or the
externally-initiated trace is rate limited, internal sampling may still choose to start or continue
the trace.

### Egress Spans

When an RPC is sent, we do **not** sample the command span, i.e., the span will only exist if a
trace already exists at the time it is created. Some reasons for this:

- We would be "double sampling" these commands, i.e., we have a chance to sample them both when the
  client sends and when the server receives.
- These provide low value for traces, as the only additional information over not keeping these
  spans is the latency between the client and the server, since the trace contains no previous spans
  from the client.

## Testing Spans

The [`traces_test_util.h`](traces_test_util.h) header provides utilities for testing that your code
correctly records spans.

### OtelTracesCapturer

`OtelTracesCapturer` installs an in-memory span exporter for the duration of its scope and captures
all spans started via `Span::start`. While the capturer is active, sampling is overridden so that
**all** spans are recorded regardless of sampling configuration. **It must be constructed before the
spans you want to capture are created.**

Because some variants do not compile OTel (notably Windows and some SUSE variants), always guard
span assertions with `OtelTracesCapturer::canReadSpans()`.

```cpp
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/traces_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::traces {

using otel::traces::Parent;
using otel::traces::HasSpanName;
using ::testing::ElementsAre;

TEST(MyFeatureTest, RecordsSpan) {
    otel::traces:::OtelTracesCapturer capturer;

    doTheOperation();

    if (!capturer::canReadSpans()) {
        GTEST_SKIP() << "OTel not configured";
    }

    EXPECT_THAT(capturer.getSpans(otel::traces::span_names::kMySpan),
                ElementsAre(Parent(HasSpanName(otel::traces::span_names::kMyParentSpan))));
}

}  // namespace mongo::otel::traces
```

### Matchers

The header provides GTest matchers for `CapturedSpan`:

| Matcher                    | Matches                                                           |
| -------------------------- | ----------------------------------------------------------------- |
| `HasSpanName(name)`        | A span whose name equals the string or `SpanName`.                |
| `HasError()`               | A span whose status was set to an error.                          |
| `HasAttribute(key, value)` | A span with an attribute `key` whose value satisfies the matcher. |
| `Parent(inner)`            | A span whose parent satisfies the inner matcher.                  |
| `Children(inner)`          | A span whose children satisfy the inner container matcher.        |

`CapturedSpan` has direct accessors for more complex scenarios.

## Build Dependencies

To create spans, depend on the `tracing` target:

```python
mongo_cc_library(
    name = "my_library",
    # ...
    deps = [
        "//src/mongo/otel/traces:tracing",
    ],
)
```

For tests using the test utilities:

```python
mongo_cc_unit_test(
    name = "my_test",
    # ...
    deps = [
        "//src/mongo/otel/traces:traces_test_util",
    ],
)
```

## Configuration

Traces are exported in [OTLP format](https://opentelemetry.io/docs/specs/otlp/) using an exporter.

- [trace_settings.idl](trace_settings.idl) contains exporter configuration and some general tracing
  parameters like resource attributes.
- [trace_sampling_parameters.idl](trace_sampling_parameters.idl) contains parameters related to
  sampling and rate limiting.
- [tracing_feature_flags.idl](tracing_feature_flags.idl) defines the feature flags.

## Key Existing Spans

- **Commands/RPCs:** A span is started with the name of the command when a command begins execution
  (ingress RPCs and local command execution) with span kind `SERVER` or `CONSUMER` (depending on the
  `moreToCome` flag). A span is started with the name of the command when an RPC is sent (egress
  RPCs) with span kind `CLIENT` or `PRODUCER` (depending on the `moreToCome` flag). These are
  sampled by default on mongos only.
