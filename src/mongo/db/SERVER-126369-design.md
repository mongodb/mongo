# SERVER-126369: Process-Wide Change-Stream Error Counters on mongoD

## Goal

Expose six process-wide counters under `serverStatus().metrics.changeStreams.errors`:

| Counter | Increment trigger |
| --- | --- |
| `totalRetriable` | Any error classified by `ErrorLabelBuilder::isResumableChangeStreamError()` (covers `isRetriableError`, `isNetworkError`, `isNeedRetargettingError`, `RetryChangeStream`, `FailedToSatisfyReadPreference`, `QueryPlanKilled`). |
| `totalNonRetriable` | Any other error terminating a `$changeStream` aggregate or getMore. |
| `historyLost` | `ErrorCodes::ChangeStreamHistoryLost` (raised in `sharded_agg_helpers.cpp:161` and `document_source_check_resume_token.cpp`). |
| `resumeTokenNotFound` | `ErrorCodes::ChangeStreamFatalError` with resume-token-not-found path (see `document_source_check_resume_token_test.cpp`). |
| `bsonObjectTooLarge` | `ErrorCodes::BSONObjectTooLarge` thrown inside a change-stream pipeline (cf. `change_stream_oplog_notification.cpp:212`). |
| `interruptedDueToReplStepChange` | `ErrorCodes::InterruptedDueToReplStateChange` raised inside a change-stream cursor lifetime. |

## Metric Site

Add to `src/mongo/db/change_stream_metrics_util.h` alongside `createCurorsTotalOpened()` and friends. Each counter uses the existing pattern:

```cpp
inline const otel::metrics::CounterOptions kErrorsTotalRetriableOpts = [] {
    otel::metrics::CounterOptions opts{};
    opts.serverStatusOptions = otel::metrics::ServerStatusOptions{
        .dottedPath = "changeStreams.errors.totalRetriable",
        .role = ::mongo::ClusterRole{::mongo::ClusterRole::None},
    };
    return opts;
}();

inline otel::metrics::Counter<int64_t>& createErrorsTotalRetriable() {
    return otel::metrics::MetricsService::instance().createInt64Counter(
        otel::metrics::MetricNames::kChangeStreamErrorsTotalRetriable,
        "Total retriable errors raised inside change-stream cursors.",
        otel::metrics::MetricUnit::kErrors,
        kErrorsTotalRetriableOpts);
}
```

Six analogous blocks plus six `MetricNames::kChangeStreamErrors*` entries (private module — added in matching CIP). `ServerStatusOptions::role = ClusterRole::None` keeps this a mongoD-only metric (matches sibling `changeStreams.cursor.*` registration).

## Increment Site

Increment from `plan_executor_pipeline.cpp` getMore / `run_aggregate.cpp` aggregate exit paths, gated on `LiteParsedPipeline::hasChangeStream()`. Classification dispatch:

1. Probe `ErrorCodes::ChangeStreamHistoryLost` → `historyLost++` then fall through to `totalNonRetriable`.
2. `ChangeStreamFatalError` with resume-token-not-found discriminator → `resumeTokenNotFound++` then `totalNonRetriable`.
3. `BSONObjectTooLarge` → `bsonObjectTooLarge++` then `totalNonRetriable`.
4. `InterruptedDueToReplStateChange` → `interruptedDueToReplStepChange++` then `totalRetriable` (it carries `isRetriableError`).
5. Else: call `ErrorLabelBuilder::isResumableChangeStreamError()`-equivalent and bump exactly one of `totalRetriable` / `totalNonRetriable`.

Sub-bucket counters are subsets of the totals. Test (below) pins `totalRetriable + totalNonRetriable == sum of all five terminal errors`.

## Feature Flag

Hidden behind `featureFlagChangeStreamErrorCounters` (mongoD-only IDL parameter, default off). When the flag is disabled, the metrics still register but never increment. Test gate: `requires_fcv_91` + `featureFlagChangeStreamErrorCounters` lookup.

## Test

`jstests/noPassthrough/observability/server_status_change_stream_error_counters.js` — replSet, drives each of the six errors via fail-points + targeted commands, asserts:

- counter present in `serverStatus().metrics.changeStreams.errors` (else `skip("counter site not landed")` — keeps the test green on the contributor branch until SERVER-126369 lands).
- monotonic increment by exactly 1 per error.
- subset invariant: `historyLost + resumeTokenNotFound + bsonObjectTooLarge + interruptedDueToReplStepChange ≤ totalRetriable + totalNonRetriable`.

## Out of Scope

- mongoS surface (counters scoped to mongoD per ticket).
- `serverStatus` schema bump (additive nested doc, no breaking change).
- Histograms or per-namespace breakdowns.
