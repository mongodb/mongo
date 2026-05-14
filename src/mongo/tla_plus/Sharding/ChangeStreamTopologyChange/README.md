# ChangeStreamTopologyChange

TLA+ specification of the change-stream V2 topology-handler state machine. Models the
interaction of `addShard` / `removeShard` / per-shard primary stepdown with a change-stream
consumer that holds a resume token (`startAtOperationTime`) and issues `getMore` against
per-shard cursors. The reference implementation is
`src/mongo/db/exec/agg/change_stream_handle_topology_change_stage.cpp` and the helper
`src/mongo/db/pipeline/change_stream_topology_helpers.cpp`.

## Scope

Actors:
- **Coordinator** — issues `addShard`, `removeShard`, primary `stepdown`; ticks cluster time.
- **Shards** — append oplog entries; carry an epoch bumped on stepdown.
- **Consumer** — opens the stream at a `startAtOperationTime` (which may be in the future
  relative to the current cluster time), opens cursors on each shard, runs `getMore` and
  advances a high-water-mark resume token.

Abstractions:
- Resume tokens are represented by their `clusterTime` field only (no UUID / event id).
- `$mergeCursors` merge-ordering is modelled as a per-shard cursor plus a global HWM the
  consumer advances post-batch.
- Replication / oplog hole-filling is not modelled; writes commit instantaneously.

## Properties

Safety:
- `NoEventBeforeResumeToken` — the consumer never delivers an event whose `clusterTime` is
  strictly less than its `startAtOperationTime`. Violated by the SERVER-48386 /
  SERVER-124540 race when a new-shard cursor opens at `shardAddedTime + 1` without comparing
  against the resume token.
- `MonotonicHighWatermark` — the consumer's HWM is bounded below by the resume token and
  bounds the cluster time of every delivered event. A complementary action property,
  `MonotonicHighWatermarkProperty`, captures non-regression across transitions.

Liveness:
- `EventuallyDelivered` — every oplog entry on an active shard with `clusterTime` at or
  above the resume token is eventually delivered. Disabled in the default `.cfg` for
  state-space budget reasons; enable via `PROPERTIES`.

## Running

```
cd src/mongo/tla_plus
./download-tlc.sh   # one-time
./model-check.sh Sharding/ChangeStreamTopologyChange
```

Default model parameters (`MCChangeStreamTopologyChange.cfg`) explore ~33k distinct states
in <2s. To reproduce the SERVER-48386 / SERVER-124540 counterexample, flip
`AllowNewShardCursorBelowResumeToken = TRUE` and re-run; TLC finds an
`NoEventBeforeResumeToken` violation at depth 4.

## Related tickets

SERVER-48386, SERVER-124540 (addShard interleaving with future `startAtOperationTime`);
SERVER-121834 (readPreference respect after elections).
