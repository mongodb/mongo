# Bulk insert + concurrent DDL atomicity

The FSM workload(s) in this directory that interleave bulk inserts with DDL operations exercise
the atomicity boundary described in
[SERVER-95924](https://jira.mongodb.org/browse/SERVER-95924): non-transactional bulk insertions
are divided into sub-batches of up to `internalInsertMaxBatchSize` documents (64 by default), and
they synchronize with DDLs only at the individual sub-batch level, not for the bulk as a whole. A
DDL that lands between two sub-batches of the same bulk can split the insertion across two
incarnations of the namespace.

The wave-1 FSM (SERVER-126543) demonstrates this empirically. The companion TLA+ specification
at

    src/mongo/tla_plus/Catalog/BulkInsertDDLAtomicity/

proves the atomicity invariant the FSM tests:

* `BulkInsertDDLAtomicity.tla` -- the model. Identifies the per-sub-batch lock as the lock-window
  (constant `LOCK_SCOPE`) and exposes the invariant `BulkInsertAtomicity`: every committed
  sub-batch of a given bulk records the same namespace incarnation.
* `MCBulkInsertDDLAtomicity.cfg` -- green configuration (`LOCK_SCOPE = "BULK"`). TLC reports no
  invariant violations: extending the lock window from sub-batch to bulk forecloses the split.
* `MCBulkInsertDDLAtomicity_bug.cfg` -- bug configuration (`LOCK_SCOPE = "SUBBATCH"`, current
  production behavior). TLC produces a counterexample trace in which a DDL fires between two
  sub-batches of the same bulk, falsifying both `BulkInsertAtomicity` and
  `NoDropIncarnationSplit`.

Run either configuration with:

    cd src/mongo/tla_plus
    ./model-check.sh Catalog/BulkInsertDDLAtomicity

To switch between configurations, copy the desired `.cfg` over `MCBulkInsertDDLAtomicity.cfg`.

The spec is intentionally minimal: it models only the lock-scope and the incarnation token,
omitting routing, shard metadata, and snapshot machinery. Routing-side effects (stale shard
version, stale db version, placement conflict time) are covered by the broader
`Sharding/TxnsCollectionIncarnation` specification; the present spec isolates the bulk-side
atomicity boundary so the counterexample directly identifies the lock window as the root cause.
