# LockFreeABA

Formal specification for the lock-free acquisition ABA hazard described in
[SERVER-76561](https://jira.mongodb.org/browse/SERVER-76561).

## The hazard

A lock-free reader receiving a request with `UNSHARDED` shard version performs
two sharding-placement checks: one before opening the storage snapshot, one
after. Both checks compare **placement state**. A concurrent shard, then drop,
then recreate-as-unsharded interleaving makes the post-snapshot check see
`UNSHARDED` again while the snapshot pins the sharded incarnation -- the
reader returns rows from the sharded collection believing it is unsharded.

The guard in [`db_raii.cpp` lines 1082-1105](
https://github.com/mongodb/mongo/blob/788248b98798044e325f30402ec3812a80dfbaf1/src/mongo/db/db_raii.cpp#L1082-L1105)
is necessary but not sufficient: placement-state equality is preserved across a
`drop + recreate-as-unsharded` cycle. The UUID-derived epoch is not.

## Model

`LockFreeABA.tla` models a single namespace with:

- A monotonic epoch counter bumped on every placement-changing DDL.
- DDLs: `ShardCollection`, `DropCollection`, `CreateUnsharded`.
- Reader state machine: `IDLE -> PRE -> SNAP -> POST -> DONE`.
- `GUARD` constant selecting `"placement"` (current, buggy) or `"epoch"`
  (proposed fix comparing snapshot epoch to post-check epoch).

## Invariants

- `LockFreeReadObservesConsistentIncarnation`: a committed read's snapshot
  epoch equals the epoch at the post-snapshot guard.
- `AttachedStateMatchesSnapshotState`: attached shard version matches the
  placement of the incarnation actually read.

Both fail under `GUARD = "placement"`; both hold under `GUARD = "epoch"`.

## Running

```
cd src/mongo/tla_plus
./model-check.sh Catalog/LockFreeABA
```

Default constants (1 reader, 3 writers, MAX_EPOCH = 4) explore the full
reachable state space in seconds.

## Companion jstest

`jstests/sharding/lock_free_acquisition_drop_recreate_aba.js` exercises the
real interleaving against a `ShardingTest` using failpoints to pin the reader
between the first placement check and the snapshot open while a writer runs
`shardCollection -> drop -> create-unsharded`.
