# Sparse-Shard Change-Stream Heartbeat

**Status:** Draft proposal, contribution against SERVER-80427.
**Owner:** Query Execution (heartbeat protocol) + Replication (oplog interaction).
**Test pin:** `jstests/change_streams/sparse_shard_resume_token_lag.js`.

## 1. Motivation

SERVER-80427 documents a long-standing complaint: in a sharded cluster, the
cluster-wide change-stream high-water-mark (the `postBatchResumeToken`, "PBRT"
hereafter) advances only when every shard reports progress. The mongos-side
merge stage (`DocumentSourceChangeStreamHandleTopologyChangeV2`, running on
`HostTypeRequirement::kRouter`) takes the minimum across per-shard cursors,
which is the only safe choice for total ordering of events. The consequence is
that a shard which sees no writes only advances its oplog via the
`NoopWriter` (`src/mongo/db/repl/noop_writer.cpp`), whose default cadence is
`periodicNoopIntervalSecs = 10s`. Consumers therefore see a 5-10s steady-state
PBRT lag on any sparse-write workload — even though the actively-written shard
is producing events at line rate. The current workaround (write a TTL'd dummy
document to every shard on a cron) is operationally awkward and trains
operators to keep `periodicNoopIntervalSecs = 1` everywhere, which has measurable
journal-IO and oplog-growth cost on otherwise-quiet shards.

## 2. Proposed Approaches

### Approach A: Server-Level Heartbeat → No-Op Oplog Writes

A new server-level thread on each primary periodically writes a typed no-op
oplog entry whenever the shard is idle, at a cadence shorter than
`periodicNoopIntervalSecs` (a new tunable, `changeStreamHeartbeatIntervalMs`,
default 500ms). The entry is shape-distinguishable from existing periodic
noops (e.g. `o: {msg: "changeStreamHeartbeat"}`) so that secondary apply, oplog
fetchers, and existing change-stream filters can ignore it cheaply, but the
oplog timestamp it produces is observable through the change-stream
high-water-mark just like any other event.

- **Pros:** Zero changes to the mongos merge path or wire protocol. Reuses the
  existing `NoopWriter` plumbing. Causality and total ordering are preserved
  by construction (the heartbeat IS an oplog entry).
- **Cons:** Write-amplification. Every shard pays a steady-state write at the
  new cadence, increasing journal IO, oplog size, and (for nodes that drive
  followers) replication network traffic. The cost is proportional to
  shard count, not load.

### Approach B: Synthetic High-Water-Mark Events at the Router

Each shard's mongod exposes a lightweight `_getResumeToken` command that
returns its current oplog timestamp without writing. Mongos polls this command
on a `changeStreamHeartbeatIntervalMs` cadence and feeds the result into the
PBRT merge logic as a synthetic, non-event "topology heartbeat" sortKey. The
resulting PBRT advances as the slowest shard's oplog timestamp advances,
regardless of whether the shard wrote.

- **Pros:** No oplog writes. Zero steady-state cost on quiet shards. The
  cadence becomes a pure router-side concern and can be tuned per cluster.
- **Cons:** Mongos must learn each shard's clock authoritatively and survive
  shard restarts, stepdowns, and topology changes; the heartbeat poll path
  becomes a new failure mode independent of the data path. Resume tokens
  produced from synthetic heartbeats must remain wire-compatible with tokens
  produced from real events so that `resumeAfter` / `startAfter` round-trip
  correctly on any release.

## 3. Recommendation: Approach A with a Targeted Optimisation

We recommend **Approach A**, on three grounds:

1. **Causality is local.** Resume tokens are oplog timestamps. Generating
   them at the shard that owns the clock avoids inventing a new distributed
   coordination protocol with its own failure modes (the well-known
   correctness gap behind several historical change-stream incidents).
2. **The write-amplification cost is bounded and tunable.** A 500ms heartbeat
   on a 3-shard cluster is six oplog entries per second, dwarfed by realistic
   workloads. Operators who want zero idle write traffic can disable the
   feature per-shard via the existing `writePeriodicNoops` parameter.
3. **Approach B re-implements something we already have.** The periodic-noop
   writer was added precisely to bridge this gap; the fix is to make it
   adaptive, not to bypass it.

The targeted optimisation: make the heartbeat write **conditional**. The
heartbeat thread only writes when `(now - lastAppliedOpTime) >
changeStreamHeartbeatIntervalMs` AND there is at least one open change-stream
cursor on the node (tracked via the existing
`CursorManager` notification on cursor registration). A shard with no open
cursors pays nothing; a shard with open cursors but active writes also pays
nothing (because `lastAppliedOpTime` is fresh). The steady-state cost falls to
zero on every shard except the genuinely-idle ones that have a consumer
waiting on them — exactly the population we are trying to serve.

## 4. Wire-Format Implications

**Additive only.** Both the heartbeat oplog entry and the PBRT it produces use
existing schemas:

- Heartbeat oplog entry: `{op: "n", o: {msg: "changeStreamHeartbeat"}}`,
  filtered out by `DocumentSourceChangeStreamCheckTopologyChange` and
  `DocumentSourceChangeStreamUnwindTransaction` exactly like the existing
  `periodic noop` entries.
- PBRT: unchanged. The resume token already carries a clusterTime; the
  heartbeat just keeps that clusterTime fresh.

A new server parameter `changeStreamHeartbeatIntervalMs` is added behind a
default-off feature flag `featureFlagChangeStreamSparseShardHeartbeat`. With
the flag off, behaviour is bit-identical to today.

## 5. Testing Strategy

1. **Regression pin (this PR):**
   `jstests/change_streams/sparse_shard_resume_token_lag.js` — a 3-shard
   cluster with writes confined to shard 0, asserts max PBRT lag < 5s. Fails
   today (default `periodicNoopIntervalSecs = 10s`); passes with the
   heartbeat enabled at 500ms.
2. **Cadence parameterisation:** sweep `changeStreamHeartbeatIntervalMs ∈
   {100, 500, 1000, 5000}` and assert the lag tracks the cadence linearly.
3. **Zero-cursor invariant:** with no open change-stream cursors on any
   shard, assert oplog growth on idle shards is unchanged versus today
   (the conditional gate must hold).
4. **Topology change interaction:** open a cluster-wide stream, add a shard
   mid-stream, assert the new shard's first heartbeat advances the PBRT
   within one cadence interval (no extra wait for the first real write on
   that shard).
5. **Resume across heartbeats:** capture a PBRT produced solely from
   heartbeat entries (no real writes on any shard during the capture window)
   and assert `resumeAfter` and `startAfter` both succeed and produce a
   stream that delivers the next real event without loss.
