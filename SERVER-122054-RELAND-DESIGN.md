# SERVER-122054 Re-Land Design — Collect Query Stats for Inserts in Standalone

**Branch:** `substrate-contrib/w3-108`
**Original commit (reverted):** `9d97bac65be0846a9aac4c48a770913bafce9e3e`
**Revert commit on master:** `113a2b495b412404308059c43d2ba5237a95611a` (auto-revert)
**Epic:** SPM-3697
**Mongos / sharded follow-up:** SERVER-122076

---

## 1. What the original change did

`#52339` wired the existing query-stats machinery (already shipped for
`find` / `update` / `count` / `distinct`) through the standalone +
replica-set `insert` command path, gated behind
`featureFlagQueryStatsInsert` (default off):

| Layer                | Change |
|----------------------|--------|
| Call site            | `performInserts` in `src/mongo/db/query/write_ops/write_ops_exec.cpp` now invokes `computeInsertShapeAndRegisterQueryStats` once before the batch loop and `collectQueryStatsMongod` once after. |
| Shape                | `InsertCmdShape` emits `command: "insert"` (parity with the other typed command shapes). |
| Shape-hash helper    | New `OperationContext` overload of `computeQueryShapeHash` in `query_shape/shape_helpers.{h,cpp}`. Insert path has no `ExpressionContext`; refactor lets the insert path share IDHACK / FLE eligibility logic with the update path. |
| Key                  | `query_stats::InsertKey` always projects `kCollection` or `kTimeseries`; `kNonExistent` is forbidden so a single logical insert is not split across the create-on-first-write boundary. |
| `execCount`          | One bump per insert command dispatched (find-style), independent of `documents.length` or retryable `stmtId` reuse. |
| Test helper          | `jstests/libs/query/query_stats_utils.js::getSlowQueryLogs` extended its `commandType` filter to also match command entries that log at the **command level** (inserts, finds), not only the nested per-statement CurOp entries that write commands previously produced. |

## 2. Why it was auto-reverted

The auto-reverter pinned a consistent failure on the
`no_passthrough_execution_control_with_prioritization` variant. The two
witness tests:

* `jstests/noPassthrough/query/queryStats/update_cmd_query_shape_hash_consistency.js`
* `jstests/noPassthrough/query/queryStats/update_cmd_mongod_slow_query_log.js`

Both tests filter mongod slow-query logs via
`getQueryShapeHashSetFromSlowLogs({..., options: {commandType: "update"}})`
and then assert size equality against the number of update statements in
the batch (e.g. `assert.eq(shardHashes.size, 3, ...)`).

### Root cause

The helper change in `query_stats_utils.js::getSlowQueryLogs` widened
the `commandType` predicate to match command-level slow log lines in
addition to the nested per-statement CurOp lines that the update path
emits. Under the prioritization variant the update path emits **both**:

1. *N* nested per-statement entries, each carrying `attr.queryShapeHash`.
2. *one* top-level command wrapper entry, also tagged `attr.type ==
   "update"`, that does **not** carry `attr.queryShapeHash`.

Before #52339, the wrapper entry was filtered out incidentally because
its shape did not match the previous (narrower) predicate. After
#52339, it now matches `commandType: "update"` — and
`getQueryShapeHashSetFromSlowLogs` asserts that *every* matching log
entry has a non-null `queryShapeHash`. The wrapper's missing hash trips
the assertion, and the size check sees one extra entry.

### Why locally green / Evergreen red

The author's `no_passthrough` runs passed locally. The wrapper-emission
behaviour observed in the prioritization variant is gated on the
`executionControlWithPrioritization` knobs (timing-sensitive duplicate
slow-log emission), which the default `no_passthrough` suite does not
exercise. The locality of the variant is what made the auto-reverter's
bisect clean — both tests flipped from green to red on a single commit
on that single variant.

## 3. Re-land plan

The production behaviour from #52339 is fundamentally sound — the
`InsertCmdShape`, `InsertKey`, `performInserts` wiring, and feature flag
remain correct. The bug lives entirely in the test helper.

### 3.1 Fix the helper

Tighten `getSlowQueryLogs` so that when the caller passes
`commandType`, write-command wrapper entries are filtered out. Two
options:

**A. Implicit: drop wrapper entries by absence of `queryShapeHash`.**

```js
// Filter by command type if specified.
if (commandType !== null && entry.attr.type !== commandType) {
    return false;
}
// When filtering by command type, exclude wrapper entries that don't
// carry a per-statement queryShapeHash. Wrappers are emitted by the
// write-command path under certain configurations (e.g. the
// executionControlWithPrioritization variant) and are distinguishable
// because attr.queryShapeHash is undefined.
if (commandType !== null && entry.attr.queryShapeHash === undefined) {
    return false;
}
```

**B. Explicit opt-in: new option `requireQueryShapeHash`.**

```js
const {includeInProgress = false, commandType = null,
       requireQueryShapeHash = false} = options;
...
if (requireQueryShapeHash && entry.attr.queryShapeHash === undefined) {
    return false;
}
```

**Recommendation: ship Option A.** Every caller of
`getQueryShapeHashFromSlowLogs` / `getQueryShapeHashSetFromSlowLogs`
already *wants* hash-bearing rows by construction — the consumer
asserts non-null hashes a moment later. Hiding wrapper entries inside
the helper keeps the test bodies readable and makes the implicit
contract ("`commandType` returns the hash-bearing rows for that
command") explicit in the helper. Option B is a fallback if any future
caller legitimately wants wrappers, which neither the existing nor the
new insert tests do.

### 3.2 Adjust insert tests defensively

The new `insert_cmd_mongod_slow_query_log.js` (re-introduced in this
re-land) lives at the **command level** — inserts log a single line per
command that *does* carry `queryShapeHash`. Under Option A, the new
helper continues to return that line. No insert-side test change
required. Under Option B, the insert tests pass
`requireQueryShapeHash: true` for symmetry.

### 3.3 Re-add the (auto-reverted) production diffs unchanged

Production diffs (write_ops_exec.cpp, insert_cmd_shape.cpp,
shape_helpers.{h,cpp}, insert_key.cpp, insert_cmd_shape_test.cpp) are
restored verbatim from commit `9d97bac65be0846a9aac4c48a770913bafce9e3e`.

### 3.4 Add a regression jstest

`jstests/noPassthrough/query/queryStats/query_stats_insert_standalone_slow_log.js`
(this PR) exercises:

* Single-document insert on a standalone produces a `$queryStats` entry
  with `command: "insert"` and a non-null `queryShapeHash`.
* Multi-document batch insert on a standalone produces **one**
  `$queryStats` row (one `execCount` bump) but `writes.nInserted == N`.
* The `queryShapeHash` emitted in mongod slow query logs (via the
  fixed-up `getSlowQueryLogs` helper) equals the `queryShapeHash` in
  `$queryStats` for the same command.
* Repeats with `commandType: "insert"` on a deployment that also fires
  the update wrapper — pins that the helper fix does not mis-attribute
  cross-command wrapper rows.
* Asserts that with `featureFlagQueryStatsInsert` disabled, no
  `$queryStats` rows are produced for inserts.

This jstest is *not* the same as the auto-reverted
`insert_cmd_mongod_slow_query_log.js` from #52339 — it is the minimum
guard that would have caught the wrapper-row contamination on the
prioritization variant before merge. The full insert test suite from
#52339 returns in a follow-up PR after the helper fix lands.

## 4. Risk assessment

| Risk | Mitigation |
|---|---|
| Helper change masks a real bug elsewhere | Option A only drops rows lacking `queryShapeHash` *when the caller asked to filter by command type*. Callers that pass no `commandType` see every slow-log row, wrappers included. |
| Reverted feature is double-disabled (flag + revert) | Default-off flag preserved; the feature stays gated on `featureFlagQueryStatsInsert` until E&R reviews the metric volume. |
| Prioritization variant still emits surprising wrappers | Out of scope; SERVER-122076 (mongos write-path) will need the same defensive helper for cross-shard wrapper emission, so this fix is a load-bearing precondition there. |

## 5. Roll-out

1. Land helper fix (Option A) as a small standalone PR — easy revert.
2. Re-introduce #52339 production diffs in a second PR depending on (1).
3. Add the regression jstest in this branch.
4. Re-enable the four jstests from #52339 in a third PR.
5. Patch all four together against `no_passthrough_execution_control_with_prioritization` before merge.

## 6. Files touched in this branch (this PR only)

* `SERVER-122054-RELAND-DESIGN.md` (this doc)
* `jstests/noPassthrough/query/queryStats/query_stats_insert_standalone_slow_log.js` (new regression test)

The production diffs and helper fix arrive in their respective PRs per
the rollout plan in §5. This branch carries only the design analysis
and the witness jstest.
