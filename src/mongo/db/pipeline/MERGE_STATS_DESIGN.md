# Design: opt-in result-reporting handle for `$merge` and `$out`

Related ticket: [SERVER-43194](https://jira.mongodb.org/browse/SERVER-43194) — "provide a way to get result/outcome of `$merge` or `$out`" (Backlog, 8 customer cases attached, Aggregation Framework component).

This document proposes an opt-in cursor option `getMergeStats: true` that asks the aggregate command to attach a small post-execution summary describing what the terminal `$merge` or `$out` stage actually did. It does **not** propose the IDL or C++ implementation; it proposes the API shape, justifies the choice of delivery channel, and points at where the counters already live so a follow-up implementation ticket can pick this up cleanly.

## 1. Motivation

Today an aggregation that ends in `$merge` or `$out` returns an empty cursor with no observability handle. The client has no programmatic way to learn:

- how many source documents were processed,
- how many target documents were matched,
- how many were inserted vs replaced vs left alone (`whenMatched: keepExisting`, `whenNotMatched: discard`),
- whether any writes failed silently (e.g. duplicate-key elision under `whenMatched: fail`).

The ticket has been open since 2019 with 5 votes, 8 customer support cases, and reports from drivers (`mongocxx` thread linked on the ticket). Operators run `$merge` jobs nightly and have to query the target collection both before and after to figure out what changed. That is a meaningful observability gap.

## 2. Proposed API

Add an opt-in field `getMergeStats: true` to the existing aggregate command's cursor options:

```javascript
db.runCommand({
    aggregate: "source",
    pipeline: [{$merge: {into: "target", whenMatched: "merge", whenNotMatched: "insert"}}],
    cursor: {getMergeStats: true}
});
```

When the terminal stage is `$merge` or `$out` and the flag is set, the server populates a new top-level field on the cursor reply:

```javascript
{
    cursor: {
        id: 0,
        ns: "db.source",
        firstBatch: [],
        mergeStats: {
            stage: "$merge",                  // or "$out"
            totalDocsProcessed: <NumberLong>, // input docs reaching the terminal stage
            matched: <NumberLong>,            // target docs matched by 'on' fields
            inserted: <NumberLong>,           // newly written
            replaced: <NumberLong>,           // overwritten existing
            discarded: <NumberLong>,          // skipped under whenNotMatched: discard
            failed: <NumberLong>              // write errors swallowed by the stage
        }
    },
    ok: 1
}
```

`mergeStats` rides alongside `cursor.postBatchResumeToken`. It is **not** inlined in `firstBatch`/`nextBatch` because those are reserved for user documents.

## 3. Wire-format change

Strictly additive. A new optional `cursor` sub-field is parsed; a new optional reply field is populated. Drivers that ignore unknown reply fields continue to work. The aggregate command's existing field set is not modified.

## 4. Backwards compatibility

The flag is opt-in and defaults to false. Without it, the cursor reply is byte-identical to today. The proposed jstest `merge_stats_handle_proposed_api.js` pins the default-off behavior so any future change that leaks stats unrequested is caught.

Wire version is **not** bumped; behavior is gated behind a feature flag (`MergeStatsHandle`, declared in IDL alongside the cursor option) so it can be promoted to GA in a normal FCV-gated rollout.

## 5. `$out` coverage

`$out` uses the same handle but populates a different counter subset. `$out` performs (a) writes to a temp collection, (b) atomic rename. The natural mapping is:

- `inserted` = documents written to the temp collection that survived the rename
- `replaced` = 0 (collection is replaced wholesale, not row-by-row)
- `matched` = 0 (no `on`-field matching)
- `discarded` = 0
- `failed` = documents that errored during the temp-collection writes

Same field set, zero-where-not-applicable. Keeps drivers' parsing code uniform.

## 6. Implementation outline

The plumbing is small because the counters already exist server-side.

- `src/mongo/db/pipeline/merge_processor.h:48` defines `struct MergeStatistics { size_t totalDocsProcessed; size_t docsMatched; ... }`. The struct lives on `MergeProcessor._mergeStats` (line 231) and is incremented inside `merge_processor.cpp` `makeUpdateStrategy()` / `makeStrictUpdateStrategy()` / `makeInsertStrategy()` (lines 120, 150, 239) every time a batch flushes.
- For the full counter set we need to (a) split `docsMatched` into `matched` / `replaced` / `inserted` by reading the strategy's `UpsertType` + the underlying `WriteResult.nUpserted` / `WriteResult.nModified`, and (b) add a `discarded` counter incremented when a `whenNotMatched: discard` / `whenMatched: discard` strategy elides a write, and (c) accumulate `failed` from the `write_ops::WriteError` vectors that the existing strategies already iterate (see `merge_processor.cpp:254`).
- `$out` lives at `src/mongo/db/exec/agg/out_stage.cpp:474` (`OutStage::flush`). Today it iterates `insert()` errors and uasserts. Add a parallel `OutStatistics` struct counting `inserted` and `failed`, populated in the same loop.
- On the cursor side, the aggregate command builds its reply in `src/mongo/db/commands/run_aggregate.cpp`. After the terminal stage finishes (`PlanExecutor::getNext()` returns `IS_EOF`), if `getMergeStats` was set and the terminal `DocumentSource` is `DocumentSourceMerge` or `DocumentSourceOut`, copy the statistics struct into a BSON sub-object and append to the cursor reply builder.
- Pass-through behavior on mongos: the merge-shard's reply already carries the stats; `cluster_aggregate.cpp` needs to either forward (if single-shard) or sum-reduce (if multi-shard) per-counter before returning to the client.

## 7. Testing strategy

Three categories, sized to fit normal `jstests/aggregation/sources/merge/` and `.../out/` suites:

1. **Stats present and correct** — for every supported `(whenMatched, whenNotMatched)` pair, exercise a known input/target combination and assert the exact counter values.
2. **Stats absent on opt-out** — assert `cursor.mergeStats === undefined` when `getMergeStats` is omitted or false. (This is what `merge_stats_handle_proposed_api.js` pins today.)
3. **Correctness under stepdown / chunk migration / multi-shard** — extend existing `noPassthrough` and sharded suites. Counters must be sum-reduced not max-reduced when the pipeline splits.

A fourth, lighter-touch category: assert that requesting `getMergeStats` on a non-`$merge`/`$out` pipeline returns the cursor reply with `mergeStats` absent (rather than erroring) — keeps the flag harmless for drivers that always set it.

## 8. Alternatives considered

- **Inline a synthetic doc in `firstBatch`.** Rejected: collides with user expectations that `$merge` / `$out` return zero documents, and would break drivers that assert `firstBatch.length === 0`.
- **Expose via `currentOp` only.** Rejected: requires the client to poll a separate command, races against op-completion, and forces a privilege check (`inprog`) that the aggregate caller may not have.
- **Write the stats into a side collection.** Rejected: requires a write the caller didn't ask for, needs a TTL story, and forces drivers into a second read.
- **Always include stats on the cursor reply.** Rejected: every aggregate cursor would carry the additional bytes whether useful or not, and the field would be ambiguously zero for pipelines without a terminal write stage. Opt-in keeps the surface area honest.
- **Return stats via `serverStatus.metrics.commands.aggregate`.** Rejected: process-level counters, not per-call.

## 9. Open questions

- Should `mergeStats` also appear on the `getMore` reply when the terminal stage runs across multiple batches? (Today `$merge`/`$out` always finish in the first batch, but if that ever changes the contract needs a definition.)
- Naming: `mergeStats` is concise but misleading for `$out`. Consider `writeStats` if API review prefers terminal-stage-agnostic naming. The jstest uses `mergeStats` consistent with this doc; trivial to rename before code lands.
- Feature flag name: proposed `MergeStatsHandle`; final name pending Query Execution team review.
