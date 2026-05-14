# TTLEmissionOrdering

A TLA+ specification of the ordering relationship between TTL-monitor
deletions and change-stream emissions on the same collection.

## Scenario

A collection has both a TTL index (deleting docs where
`createdAt + expireAfterSeconds <= now`) and a change-stream watcher. The
spec models three actors:

- **`USER_WRITER`**: inserts a document, stamping `createdAt`. The insert
  first lands in a `staged` state, then a `CommitInsert` action appends the
  oplog entry and moves the doc to `committed`.
- **`TTL_MONITOR`**: scans for expired docs (`createdAt + ExpireAfter <=
  clock`) and deletes them, appending a `delete` event to the oplog.
- **`STREAM_CONSUMER`**: pulls events from a change-stream cursor pinned at
  `ResumeToken`, applies a `$match` filter (`ConsumerMatchSet`), and reads
  what survives.

Two model knobs gate the bug paths:

- `AllowTTLBeforeCommit = TRUE` lets the TTL monitor target a `staged`
  (uncommitted) doc, producing an oplog whose delete entry precedes the
  insert entry.
- `AllowOutOfOrderEmit = TRUE` lets the change-stream cursor skip over an
  oplog entry that has not yet been emitted, modelling a $match
  misconfiguration where the insert is filtered but the delete is not.

## Invariants

- **`CausalOrdering`**: every observed `delete` for doc `d` is preceded
  (in the consumer's trace) by an observed `insert` for `d`, OR the
  insert was below the resume token, OR `d` was filtered by `$match`.
- **`NoOrphanDelete`**: stricter form — the consumer never sees a delete
  for a doc whose insert it didn't see, except for resume-token-skipped
  inserts.
- **`NoDeleteBeforeInsertInOplog`**: the oplog itself never carries a
  delete for a doc whose insert appears later (or never).
- **`TTLNoSkip`** (liveness): every doc that becomes TTL-eligible is
  eventually deleted (no silent skip).
- **`EventuallyDrained`** (liveness): every oplog event past the resume
  token and the `$match` filter eventually reaches the consumer.
- **`OplogMonotonic`**: oplog clusterTimes are non-decreasing.

With both bug knobs `FALSE`, all six invariants hold across the bounded
state space. Setting `AllowTTLBeforeCommit = TRUE` produces a
counterexample trace for `NoDeleteBeforeInsertInOplog` and (under the
right interleaving) for `CausalOrdering`.

## Running

From `src/mongo/tla_plus`:

```
./download-tlc.sh   # once, fetches tla2tools.jar
./model-check.sh ChangeStreams/TTLEmissionOrdering
```

State space is bounded by `StateBound` in `MCTTLEmissionOrdering.tla`;
the headline configuration finishes well under a minute on a laptop.
