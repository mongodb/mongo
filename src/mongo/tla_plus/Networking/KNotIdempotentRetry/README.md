# KNotIdempotentRetry

TLA+ spec for [SERVER-108482][s108482]: the `Shard::RetryPolicy::kNotIdempotent`
classifier retries on the full `ErrorCodes::isNotPrimaryError` category, but
`error_codes.yml` documents that category as insufficient to determine whether
a write occurred. PR [#52940][pr52940] removed the entire category and was
reverted in [#53185][pr53185] because the all-or-nothing change broke tests
that rely on retrying after retargeting.

## What the spec models

Three orthogonal axes per request, intersected against the classifier:

1. `RequestState` ∈ {`PreSend`, `SentUnknown`, `Applied`, `NotApplied`}
2. `NotPrimarySub` ∈ {`NotWritablePrimary`, `NotPrimaryNoSecondaryOk`,
   `PrimarySteppedDown`, `InterruptedDueToReplStateChange`, `NoNotPrimary`}
3. `IdemClass` ∈ {`NonIdempotentWrite`, `NonIdempotentNonWrite`, `Idempotent`}

The classifier is gated by the bug toggle `AllowRetryOnAmbiguousNotPrimary`:

- `FALSE` → retry only on `RejectedBeforeApplySubset`
  = {`NotWritablePrimary`, `NotPrimaryNoSecondaryOk`}.
- `TRUE` → retry on the full `NotPrimaryError` category
  (the currently-live post-revert behaviour).

The safety invariant `NoDoubleApplication` asserts that any non-idempotent
operation is applied on the primary at most once across the whole retry
history.

## The minimum-safe NotPrimary subset

Two semantic groups inside the `NotPrimaryError` category:

| Subclass | Apply path reached? | Safe to retry as kNotIdempotent? |
|----------|---------------------|----------------------------------|
| `NotWritablePrimary`             | No, rejected at command entry  | Yes |
| `NotPrimaryNoSecondaryOk`        | No, rejected before dispatch   | Yes |
| `PrimarySteppedDown`             | Possibly, stepdown mid-flight  | No  |
| `InterruptedDueToReplStateChange`| Possibly, kill mid-apply       | No  |

`RejectedBeforeApplySubset` (the first two) is the minimum-safe set: the
server guarantees no side effect occurred before returning these. The other
two are the ambiguous set the original ticket flags and are exactly what
`NoWritesPerformed` (SERVER-66479) was designed to disambiguate.

This is also why PR #52940's all-or-nothing revert is wrong in both
directions: keeping all four breaks `NoDoubleApplication`; dropping all four
breaks `RunUserManagementWriteCommandNotWritablePrimaryRetrySuccess`, which
legitimately retries after retargeting.

## Running

```sh
cd src/mongo/tla_plus
./download-tlc.sh
./model-check.sh Networking/KNotIdempotentRetry          # green: invariant holds
# To reproduce the bug counterexample, swap MCKNotIdempotentRetry.cfg with
# MCKNotIdempotentRetry-bug.cfg and re-run; TLC will print a trace where a
# non-idempotent write is Applied twice.
```

[s108482]: https://jira.mongodb.org/browse/SERVER-108482
[pr52940]: https://github.com/10gen/mongo/pull/52940
[pr53185]: https://github.com/10gen/mongo/pull/53185
