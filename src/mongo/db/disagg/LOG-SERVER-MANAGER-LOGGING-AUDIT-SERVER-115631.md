# LogServerManager Logging Audit (SERVER-115631)

Pure observability audit. The goal is to make every `LogServerManager`
phase narrate itself in structured logs at a verbosity that is debuggable
but not log-spam. PALI runs on every node touching disaggregated log
storage, so every line added here is amortised across the fleet — debug
levels and rate limiting are mandatory wherever the call-site fires per
write or per fan-out batch.

## Phases of LogServerManager

1. **Registration.** A node learns the peer set from the catalog, builds
   its `peerId -> cellId` map, and registers itself as either a writer
   replica or a follower for each cell. Failures here are silent today —
   we only notice when a downstream append times out.
2. **Connection-pool establishment.** For each registered peer, the
   manager opens (or reuses) a pooled gRPC/egress channel, runs the
   handshake, and stamps the connection with the negotiated config
   version. Pool churn is invisible above DEBUG2.
3. **Stream lifecycle.** Per-stream state machine: `Idle ->
   Connecting -> Healthy -> Degraded -> Draining -> Closed`. Transitions
   currently log only at `Closed`, and only the terminal reason.
4. **Failover.** On peer loss or stale-config detection, the manager
   reassigns the writer role, re-shards the in-flight append queue, and
   restarts streams against the new primary cell. We log the decision
   but not the inputs that produced it.
5. **Shutdown.** Drain append queues, flush pending fan-outs, close
   streams, deregister from the catalog. Today this is a single
   "shutdown complete" line; partial-drain failures are swallowed.

## Missing-log categories

- **Per-stream state transitions.** Every `prev -> next` edge should
  emit one line at INFO when crossing a health boundary
  (`Healthy <-> Degraded`, `* -> Closed`) and DEBUG2 for internal edges
  (`Connecting -> Healthy`). Carry the trigger (heartbeat-miss,
  explicit-close, stale-config, peer-removed).
- **Append-log queue-depth at key thresholds.** Sample-and-log on
  crossings of 25%, 50%, 75%, 90%, 100% of `kMaxQueueDepth` rather than
  every enqueue. Emit once per crossing per direction (rising/falling)
  with a 5s debounce per stream.
- **Fan-out batch acceptance.** One DEBUG1 line per accepted batch with
  `batchId`, `batchBytes`, `entryCount`, `replicaCount`, and the elapsed
  ms from enqueue to acceptance. Rate-limit to 1-in-N (configurable,
  default N=64) so steady state doesn't drown the log.
- **Fan-out batch rejection.** Always log at WARNING with a structured
  `reason` enum: `QueueFull`, `StaleConfig`, `PeerUnreachable`,
  `QuorumLost`, `Backpressure`, `ChecksumMismatch`. No rate limiting on
  rejections — they are rare and load-bearing for incident triage.
- **Peer-id-to-cell-id mapping changes.** Every diff between the
  previous and new mapping logs at INFO with explicit `added[]`,
  `removed[]`, `relocated[]` arrays. Mapping diffs are infrequent and
  load-bearing across failover post-mortems.
- **Stale-config detection.** Emit at WARNING the first time per
  `(stream, configVersion)` pair that the manager observes a remote
  version newer than its own, with `localConfigVersion`,
  `remoteConfigVersion`, and the `detectingPath` (heartbeat /
  append-response / catalog-refresh). Suppress duplicates until the
  catch-up completes.

## Structured-attribute name recommendations

Use `logv2` BSON attributes consistently across phases:

- Registration: `peerId`, `cellId`, `role`, `configVersion`,
  `catalogEpoch`, `outcome`.
- Connection pool: `peerId`, `endpoint`, `handshakeMs`, `poolSize`,
  `reuseCount`, `negotiatedConfigVersion`.
- Stream lifecycle: `streamId`, `peerId`, `cellId`, `prevState`,
  `nextState`, `trigger`, `durationInPrevStateMs`.
- Append queue: `streamId`, `queueDepth`, `queueCapacity`,
  `crossingThresholdPct`, `direction` (`rising`/`falling`).
- Fan-out accept: `batchId`, `streamId`, `entryCount`, `batchBytes`,
  `replicaCount`, `acceptedMs`, `sampledOneIn`.
- Fan-out reject: `batchId`, `streamId`, `reason`, `entryCount`,
  `inFlightBatches`, `queueDepth`, `remoteConfigVersion`.
- Mapping change: `epochBefore`, `epochAfter`, `added`, `removed`,
  `relocated`, `triggeredBy`.
- Stale config: `streamId`, `localConfigVersion`,
  `remoteConfigVersion`, `detectingPath`, `firstObservedAtMs`.

Reusing the same attribute names across phases keeps mongotail / log-
shipping aggregations cheap and Atlas LogIngest indexable.

## Threshold knobs (debug vs always-log)

| Event | Level | Rate limit |
|---|---|---|
| Registration outcome | INFO | none |
| Connection handshake success | DEBUG1 | none |
| Connection handshake failure | WARNING | none |
| Stream transition (health boundary) | INFO | none |
| Stream transition (internal) | DEBUG2 | none |
| Queue depth threshold crossing | INFO | 1 per stream per 5s |
| Fan-out batch accepted | DEBUG1 | 1-in-N, N=64 default |
| Fan-out batch rejected | WARNING | none |
| Peer/cell mapping change | INFO | none |
| Stale-config first observation | WARNING | 1 per (stream, version) |
| Shutdown phase boundaries | INFO | none |

Knobs ride on `logv2` component verbosity for `kPALI` plus a
`logServerManagerFanoutSampleRate` server parameter (default 64) so the
SRE can lift sampling during incidents without a restart.

## Follow-ups

- Wire a `LogServerManagerStats` server-status section so queue-depth
  and rejection counts are diff-able in FTDC alongside the new logs.
- Add a unit test that pins the structured-attribute names so log-
  shipping consumers don't silently break on rename.
