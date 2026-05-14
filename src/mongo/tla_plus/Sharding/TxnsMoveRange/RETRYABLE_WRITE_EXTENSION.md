# Retryable-Write Session-Table Extension to TxnsMoveRange

This note sketches how `TxnsMoveRange.tla` would be extended (as a sibling spec, not a
replacement) to model the retryable-write session-table migration that backs SERVER-54019.
The existing spec covers multi-statement-transaction placement correctness during `moveChunk`;
it does not model `config.transactions` rows being copied from donor to recipient as part of
session migration, which is the surface SERVER-54019 lives on.

## Scope

The extended spec would model a single retryable update batch with `ordered: false`, two
update-ones by `_id` without the shard key, and one chunk migration interleaved between the
original batch and a retry of the same `(lsid, txnNumber)`. The correctness property to
verify is that **after the retry, the sum across shards of `(n, nModified)` returned to the
router for any given `stmtId` is at most one**, regardless of where session-table rows have
been copied.

## New state variables

Added on top of the existing `vars` tuple in `TxnsMoveRange.tla`:

- `shardSessionTable[s][lsid][txnNum][stmtId] \in {NotSeen, Applied}` — per-shard
  `config.transactions` projection, keyed by logical session id, transaction number, and
  statement id. `Applied` records the `(n, nModified)` outcome reported by that shard on the
  initial pass.
- `shardStmtResult[s][lsid][txnNum][stmtId] \in [n: 0..1, nModified: 0..1]` — the per-shard
  result that gets cached alongside the `Applied` marker (this is what the bug surfaces:
  the recipient inherits the donor's row and so "remembers" `n=1` even though it never owned
  the matching `_id`).
- `routerStmtSum[lsid][txnNum][stmtId] \in [n: 0..numShards, nModified: 0..numShards]` —
  the router-side summed response across shards. The correctness property is stated against
  this variable.
- `sessionMigrationInFlight[from, to] \in BOOLEAN` — gating predicate so a `MoveRange`
  action and its paired session-table copy step are not interleaved with unrelated traffic.

## New actions

- `ShardApplyRetryableStmt(s, t, lsid, txnNum, stmtId)` — extends `ShardRespond`. If the
  shard's `shardSessionTable[s][lsid][txnNum][stmtId] = NotSeen`, the shard either matches
  the predicate (records `Applied` with `[n |-> 1, nModified |-> 1]`) or does not (records
  `Applied` with `[n |-> 0, nModified |-> 0]`). If already `Applied`, replay returns the
  cached result with no document mutation.
- `MoveRangeWithSessionMigration(ns, k, from, to)` — supersedes `MoveRange` for this spec.
  After the existing range-ownership flip, copies the donor's
  `shardSessionTable[from][lsid][...]` into `shardSessionTable[to][lsid][...]` *for every
  `stmtId` the donor has marked `Applied`*, including those whose cached result is `n: 0`
  (this is the bug surface — the recipient is left "remembering" that stmtId was applied,
  even when the recipient never owned the matching `_id`).
- `RouterAggregateRetry(lsid, txnNum, stmtId)` — sums `shardStmtResult` across all shards
  whose `shardSessionTable` entry is `Applied` for this `(lsid, txnNum, stmtId)`, writes the
  sum into `routerStmtSum`.

## New invariants

- `NoInflatedRetry` — `\A lsid, txnNum, stmtId : routerStmtSum[lsid][txnNum][stmtId].n <= 1
  /\ routerStmtSum[lsid][txnNum][stmtId].nModified <= 1`. This is the SERVER-54019 invariant;
  the unfixed model is expected to produce a TLC counterexample violating it within ≤6
  steps (apply → respond → migrate → copy-session → retry → aggregate).
- `SessionTableCopyMonotone` — `MoveRangeWithSessionMigration` only ever transitions
  `NotSeen → Applied` on the recipient; never overwrites a recipient's existing entry.

## What the counterexample buys

A TLC run against the unfixed model should produce the exact 6-step trace that mirrors
the jstest reproducer at `jstests/sharding/retryable_write_session_migration_inflated_n.js`.
A fix candidate (e.g. tagging recipient session rows with a "donor-only" flag that
suppresses contributions to `routerStmtSum` when the recipient never owned the matching
key) can then be added as a guarded variant of `MoveRangeWithSessionMigration` and
re-checked; passing TLC + passing the jstest closes the loop.

The extended spec would live as `TxnsMoveRangeWithSessionMigration.tla` plus its own
`MCTxnsMoveRangeWithSessionMigration.cfg` sibling under
`src/mongo/tla_plus/Sharding/TxnsMoveRange/`, so the original spec stays intact as the
canonical reference for transaction-placement correctness.
