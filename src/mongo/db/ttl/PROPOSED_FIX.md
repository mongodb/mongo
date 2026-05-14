# Proposed fix for SERVER-92779 — TTL delete progress blocked by unowned documents

## Symptom

On a sharded collection, when a shard physically holds more than
`ttlIndexDeleteTargetDocs` (default `50000`) expired *orphan* documents whose
shard-key position in the TTL index precedes an owned expired document, the TTL
monitor never deletes the owned document. The collection grows unbounded until
the orphans are cleaned up by some other mechanism (e.g. range deleter).

## Root cause

The TTL monitor delegates batched deletion to `BatchedDeleteStage`. Two
counters control when a pass ends:

- `_batchTargetMet()` — buffer-level: triggers a commit attempt when the buffer
  reaches `targetBatchDocs` documents or `targetStagedDocBytes` bytes.
- `_passTargetMet()` — pass-level: declares "we're done for this pass" when
  `_passTotalDocsStaged >= targetPassDocs`.

`_passTotalDocsStaged` is incremented for **every staged document**
(`batched_delete_stage.cpp:516`) — including documents the commit phase later
skips because the shard does not own them
(`batched_delete_stage.cpp:389-394`).

Commit-phase orphan skips happen because `BatchedDeleteStage` consults the
collection's ownership filter on commit, but the staging loop above sees only
the raw index cursor.

Result: a shard with `N >= targetPassDocs` expired orphans ordered earlier in
the TTL index than a single owned expired document will stage `N` orphans,
trip `_passTargetMet()` without issuing any real delete, mark
`_passStagingComplete = true`, drain (skipping every staged orphan), and return
EOF for the pass before ever reaching the owned document. The next TTL pass
restarts from the same index position and produces the same result. Forward
progress = 0.

Note: the ticket description names a related variant — expired orphan
documents on a *recipient* shard (chunk received but not yet committed) can
also block TTL progress through the same accounting path. The fix proposed
below addresses both: it discounts any staged document the ownership filter
will reject regardless of whether the shard is a donor or a recipient.

## Proposed fix

Two layered changes; either is sufficient on its own, but applying both is the
cleanest and most defensive option.

### Change A (preferred) — don't charge orphans against `targetPassDocs`

In `BatchedDeleteStage::_deleteBatch` (where the per-document
`isDocOwnedByShard` / ownership-filter check fires and the document is added
to `recordsToSkip`), decrement `_passTotalDocsStaged` for every skipped
record. The pass target then reflects work the stage actually *committed* (or
genuinely attempted), not work the stage was structurally forbidden from
performing.

```diff
--- a/src/mongo/db/exec/classic/batched_delete_stage.cpp
+++ b/src/mongo/db/exec/classic/batched_delete_stage.cpp
@@ -385,6 +385,14 @@ PlanStage::StageState BatchedDeleteStage::_deleteBatch(WorkingSetID* out) {
             // Skip documents that are no longer owned by this shard.
             if (!_isDocStillOwnedByShard(opCtx(), member->doc.value())) {
                 recordsToSkip.insert(_stagedDeletesBuffer.at(...));
+                // SERVER-92779: orphans must not count against the per-pass
+                // document target. Without this decrement, a shard holding
+                // >= targetPassDocs expired orphans never reaches owned
+                // expired documents stored later in the TTL index, because
+                // _passTargetMet() trips on staged-but-skipped orphans and
+                // the pass returns EOF before any real delete is issued.
+                invariant(_passTotalDocsStaged > 0);
+                --_passTotalDocsStaged;
                 continue;
             }
```

This is a one-counter accounting fix and changes no contract: `targetPassDocs`
already means "approximate target", and the comment in `ttl.idl` already says
"Limits (approximately) the number of expired documents *removed*". With this
change the counter actually counts removals (or honestly-attempted removals),
not skipped pre-flight stagings.

### Change B (defense in depth) — TTL monitor retries on a pass that staged but deleted zero

In `TTLMonitor::_deleteExpiredWithIndex`
(`src/mongo/db/ttl/ttl_monitor.cpp:564`), after the delete executor returns
its batched-delete stats, treat the "examined > 0, deleted == 0,
passTargetMet == true" combination as "there is more to do" and return
`true`, prompting the caller (`_doTTLSubPass`) to schedule another sub-pass on
this index. This guards against future regressions where another commit-time
skip path bypasses Change A's accounting.

```diff
--- a/src/mongo/db/ttl/ttl_monitor.cpp
+++ b/src/mongo/db/ttl/ttl_monitor.cpp
@@ -667,7 +667,16 @@ bool TTLMonitor::_deleteExpiredWithIndex(...) {
         if (batchingEnabled) {
             auto batchedDeleteStats = exec->getBatchedDeleteStats();
-            // A pass target met implies there may be more to delete.
-            return batchedDeleteStats.passTargetMet;
+            // A pass target met implies there may be more to delete.
+            // SERVER-92779: additionally, if we examined documents but
+            // deleted none and the pass target tripped, we likely stalled on
+            // a band of orphans that the batched delete stage skipped at
+            // commit time. Force another sub-pass so the TTL monitor walks
+            // past the orphan-saturated region.
+            const bool stagnated =
+                summaryStats.totalDocsExamined > 0 && numDeletedDocs == 0 &&
+                batchedDeleteStats.passTargetMet;
+            return batchedDeleteStats.passTargetMet || stagnated;
         }
```

## Tests

- `jstests/sharding/ttl_blocked_by_unowned_docs.js` (added in this branch) —
  reproduces the stall against today's tree (currently early-returns until the
  fix lands), and asserts forward progress + the existing
  "TTL doesn't delete orphans" invariant when `FIX_LANDED = true`.
- Suggested unit-level addition: `src/mongo/db/exec/classic/batched_delete_stage_test.cpp`
  case that wires a mock ownership filter rejecting half the staged docs and
  asserts `_passTargetMet()` accounts only for owned stages.

## Risk

Behavior change is confined to the orphan-skip path. On collections with no
orphans, neither change has any observable effect (`recordsToSkip` is empty,
`numDeletedDocs > 0` whenever `totalDocsExamined > 0`). On collections with
orphans the only behavioral change is "TTL now makes forward progress past
the orphan band" — the intended fix.
