# `$merge` Safe-Retry Classification under Config Transitions

**Ticket:** SERVER-99827 — *Exclude `$merge` tests from config-transition suites due to unsafe retries.*
**Companion:** SERVER-126537 (wave-1) shipped the stepdown-idempotency matrix. This document narrows that matrix to **safe-retry classification** per `whenMatched × whenNotMatched` cell and proposes a resmoke filter delta.

## Background

A `$merge` stage decomposes into per-document writes against the target namespace, governed by a pair of mode parameters defined in `document_source_merge_modes.idl`:

- `whenMatched ∈ {fail, keepExisting, merge, replace, <pipeline>}`
- `whenNotMatched ∈ {discard, fail, insert}`

Under a config transition (`config_shard ↔ dedicated_config_server`), individual writes inside the `$merge` batch may be retried after a stepdown/transition boundary. The retry is **safe** iff replaying the same write at-least-once yields the same final target state as executing it exactly-once. The two failure surfaces we care about:

1. **Duplicate-insert surface** — the second attempt re-inserts a document already inserted by the first attempt's now-orphaned write.
2. **Non-idempotent update surface** — `whenMatched: <pipeline>` whose stages reference the existing target document non-monotonically (e.g. `{$inc: ...}` inside an `$addFields`, counter accumulation, append-to-array).

## Safe-retry classification table

Twelve actionable cells (the internal `kPipeline` enum sentinel is collapsed into the `<pipeline>` row, since the string `"pipeline"` is not user-addressable per the IDL comment):

| whenMatched ↓ \ whenNotMatched → | `discard`   | `fail`      | `insert`    |
| -------------------------------- | ----------- | ----------- | ----------- |
| `fail`                           | SAFE        | SAFE\*      | UNSAFE      |
| `keepExisting`                   | SAFE        | SAFE\*      | UNSAFE      |
| `merge`                          | SAFE        | SAFE\*      | UNSAFE      |
| `replace`                        | SAFE        | SAFE\*      | UNSAFE      |
| `<pipeline>`                     | COND-SAFE   | COND-SAFE\* | UNSAFE      |

**Legend:**
- **SAFE** — retry collapses to a deterministic upsert/no-op. The first attempt either touched the target document or didn't; the second attempt observes the post-first-attempt state and reaches the same fixpoint.
- **SAFE\*** — safe iff the `fail` branch never fires on the retry path. If the first attempt already inserted/updated the matching document and the retry now sees a match where the original saw none (or vice versa), `fail` will surface a stepdown-only error the user did not write the test to catch. **Treated as UNSAFE for suite-tag purposes** but factually idempotent on the success path.
- **UNSAFE** — the `insert` branch of `whenNotMatched` reissues the insert on retry. With a non-`_id` `on:` field set, the first attempt's insert is invisible to the retry's match-predicate evaluation (chunk migration races, secondary lag), producing a duplicate row. Even with `on: ["_id"]`, the duplicate-key error surfaces only on shards where the chunk has just moved, which is exactly what config-transition suites exercise.
- **COND-SAFE** — pipeline mode is safe iff every stage in the update pipeline is **idempotent under composition with itself**: `f(f(x)) = f(x)`. `$set`, `$replaceRoot`, `$project`, `$unset` qualify; `$addFields` with `$add`/`$multiply`/`$concatArrays` over the existing field does not. The classifier cannot decide this statically without parsing the pipeline; tests opting into pipeline mode under config transition must self-attest via the new tag `assumes_idempotent_merge_pipeline`.

## Proposed resmoke filter delta

Add one tag to `jstests/aggregation/sources/merge/mode_*_insert.js` and to any other test that constructs a `$merge` stage with `whenNotMatched: "insert"`:

```
// @tags: [
//   does_not_support_config_shard_transitions,
//   ...existing tags...
// ]
```

Then in the config-transition resmoke suite YAML (`buildscripts/resmokeconfig/suites/config_shard_transitions_jscore_passthrough.yml` and siblings under `*_transition_*.yml`), add to `selector.exclude_with_any_tags`:

```yaml
selector:
  exclude_with_any_tags:
    - does_not_support_config_shard_transitions
```

This single tag covers all UNSAFE cells. The SAFE\* cells (rows 1–4, `whenNotMatched: fail`) are left in-suite because the `fail` branch firing on retry is a legitimate detection signal users want from the production behaviour; flake risk there is bounded by `assert.commandFailedWithCode` already in the test bodies.

For COND-SAFE pipeline mode, introduce a second tag — `assumes_idempotent_merge_pipeline` — gated by the same exclude list. Tests that genuinely use idempotent pipelines (e.g. `mode_pipeline_discard.js`) drop the tag; tests that use accumulating pipelines keep it. This matches the ticket's request to *avoid duplicating exclude lists* — the global suite YAML references the tag once, and per-file tags are the single source of truth.

## Companion jstest extension (optional)

A new file `jstests/aggregation/sources/merge/safe_retry_classification.js` exercises the matrix programmatically: iterate the eight `(whenMatched, whenNotMatched)` user-addressable pairs against a small standalone target, force a stepdown via `replSetStepDown` between two writes of the same `$merge` batch, and `assertArrayEq` the post-retry target against the once-only baseline. The five `UNSAFE` cells assert `commandFailedWithCode(DuplicateKey)`; the five `SAFE/SAFE*` cells assert `commandWorked` and array-equality. Tagged `requires_replication, does_not_support_config_shard_transitions`.

## Provenance

- Wave-1 reference: `SERVER-126537` stepdown-idempotency matrix.
- IDL source of truth: `src/mongo/db/pipeline/document_source_merge_modes.idl` (`MergeWhenMatchedMode`, `MergeWhenNotMatchedMode`).
- Behavioural source of truth: `src/mongo/db/pipeline/merge_processor.cpp`.
- Ticket: SERVER-99827 (Adi Agrawal, Query Execution, P3, Backlog).
