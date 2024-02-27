# Open telemetry (OTel) in resmoke

OTel is one of two systems we use to capture metrics from resmoke. For mongo-tooling-metrics please see the documentation [here](README.md).

## What Do We Capture

Using OTel we capture the following things

1. How long a resmoke suite takes to run (a collection of js tests)
2. How long each test in a suite takes to run (a single js test)
3. Duration of hooks before and after test/suite
4. Resmoke archiver (when there is a failure we archive core dumps)

To see this visually navigate to the [resmoke dataset](https://ui.honeycomb.io/mongodb-4b/environments/production/datasets/resmoke/home) and view a recent trace.

## A look at source code

### Configuration

The bulk of configuration is done in the
`_set_up_tracing(...)` method in [configure_resmoke.py#L164](https://github.com/10gen/mongo/blob/976ce50f6134789e73c639848b35f10040f0ff4a/buildscripts/resmokelib/configure_resmoke.py#L164). This method includes documentation on how it works.

## BatchedBaggageSpanProcessor

See documentation [batched_baggage_span_processor.py#L8](https://github.com/mongodb/mongo/blob/976ce50f6134789e73c639848b35f10040f0ff4a/buildscripts/resmokelib/utils/batched_baggage_span_processor.py#L8)

## FileSpanExporter

See documentation [file_span_exporter.py#L16](https://github.com/10gen/mongo/blob/976ce50f6134789e73c639848b35f10040f0ff4a/buildscripts/resmokelib/utils/file_span_exporter.py#L16)

## Capturing Data

We mostly capture data by using a decorator on methods. Example taken from [job.py#L200](https://github.com/10gen/mongo/blob/6d36ac392086df85844870eef1d773f35020896c/buildscripts/resmokelib/testing/job.py#L200)

```
TRACER = trace.get_tracer("resmoke")

@TRACER.start_as_current_span("func_name")
def func_name(...):
    span = trace.get_current_span()
    span.set_attribute("attr1", True)
```

This system is nice because the decorator captures exceptions and other failures and a user can never forget to close a span. On occasion we will also start a span using the `with` clause in python. However, the decorator method is preferred since the method below makes more of a readability impact on the code. This example is taken from [job.py#L215](https://github.com/10gen/mongo/blob/6d36ac392086df85844870eef1d773f35020896c/buildscripts/resmokelib/testing/job.py#L215)

```
with TRACER.start_as_current_span("func_name", attributes={}):
    func_name(...)
    ...
```

## Insights We Have Made (so far)

Using [this dashboard](https://ui.honeycomb.io/mongodb-4b/environments/production/board/3bATQLb38bh/Server-CI) and [this query](https://ui.honeycomb.io/mongodb-4b/environments/production/datasets/resmoke/result/GFa2YJ6d4vU/a/7EYuMJtH8KX/Slowest-Resmoke-Tests) we can see the most expensive single js tests. We plan to make tickets for teams to fix these long running tests for cloud savings as well as developer time savings.
