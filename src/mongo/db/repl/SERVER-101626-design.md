# SERVER-101626 — Prepared transaction secondary application throughput

## Background

The replication oplog applier on secondaries processes oplog entries in
parallel batches; CRUD ops on disjoint documents fan out across the writer
thread pool and apply concurrently. Prepared transactions break that
parallelism. Two structural facts cause it:

1. `applyPrepareTransaction` materialises the prepared transaction inside a
   per-session `TransactionParticipant` and takes an exclusive lock against
   the secondary's session store while it does so. The session store is a
   process-wide map keyed by `lsid`; the lock is taken to prevent concurrent
   apply paths from racing on the same session's participant state, but in
   practice it serialises *every* prepare applied in a given batch — not
   just prepares targeting the same session.
2. SERVER-75800 (7.0/8.0) introduced batching of prepare and commit oplog
   entries on secondaries, but prepares and commits are not batched
   *together*, and commit-prepared cannot batch with surrounding CRUD ops.
   The effect is that a workload with even modest prepared-txn fan-in
   produces a single-writer choke point on secondaries while the rest of
   the writer pool stalls waiting for the session-store lock.

Under cross-shard transactional load (the original reporter context) this
serialisation dominates secondary application latency and shows up as
ballooning cross-shard commit times even when the primary easily absorbs
the inbound prepare rate.

## What this change adds

This change does **not** fix the bottleneck. It ships:

- `jstests/replsets/prepared_txn_secondary_throughput.js`, a regression-pin
  that drives N=10 concurrent prepared transactions on a 3-node
  `ReplSetTest`, measures the wall-clock time for secondaries to fully
  apply every prepare via `awaitReplication()`, sanity-checks via oplog
  tailing that all N prepares actually landed, and asserts a generous
  upper bound (60s) so the current single-writer behaviour is documented
  rather than regressed silently. When the parallel-applier fix lands the
  measured wall time should drop and the bound should be tightened.
- This design note.

N=10 is intentionally modest: the choke is observable well below the point
where the test would OOM shared CI infrastructure, and a small N keeps the
test reliably under the per-evergreen-task wall-clock budget.

## Fix sketch (out of scope for this change)

The fix shape we expect to land separately:

1. **Split the session store lock.** The exclusive lock on the session
   store today protects map-level invariants, not per-`TransactionParticipant`
   state. Replace it with a sharded lock keyed by `lsid` (or a striped
   `Mutex` array) so that two prepares for different sessions can be
   applied concurrently. Per-session correctness is already enforced by the
   `TransactionParticipant`'s own mutex.
2. **Teach the writer pool to fan out prepare entries.** The applier's
   batch scheduler currently routes all prepare entries to a single writer
   thread to avoid races on the session store. Once (1) is in place the
   scheduler can shard prepares by `lsid` hash the same way it shards CRUD
   ops by `_id` hash.
3. **Allow commit-prepared to batch with surrounding CRUD ops** where the
   write sets are disjoint from any still-pending prepared transaction.
   This reclaims the parallelism that SERVER-75800 left on the table.

Step (1) is the load-bearing change; (2) and (3) are mechanical follow-ups
that unlock the observable throughput gain. The regression-pin landed here
will measure that gain when it ships.
