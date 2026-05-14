# Oplog-Volume-Triggered Checkpoint Cadence

**Ticket:** [SERVER-120876](https://jira.mongodb.org/browse/SERVER-120876)
**Status:** In Code Review
**Authors:** Surya Dhoolam (impl), substrate-contrib (design doc + jstest)
**Related:** [SERVER-114491](https://jira.mongodb.org/browse/SERVER-114491) (DSC checkpoint-write workaround to revert)

## 1. Problem

`Checkpointer::run()` sleeps for `storageGlobalParams.syncdelay` seconds, then
unconditionally calls `StorageEngine::checkpoint()`. ASC defaults `syncdelay` to
60s; DSC defaults to 5s. For low-throughput DSC workloads we have observed
checkpoint-driven write amplification 75x larger than the underlying oplog
write rate. Frequent checkpoints exist to bound recovery time on non-primary
nodes; a quiet node does not need them.

## 2. Goals

- Decouple checkpoint cadence from a single fixed interval.
- Cheap checkpoints when oplog has accumulated meaningful work
  (`>= checkpointOplogVolumeBytes`, recommended default 16 MB).
- Lower bound `checkpointMinIntervalSecs` (default 5s) — never thrash.
- Upper bound `checkpointMaxIntervalSecs` (default 300s) — bound recovery RPO.
- Preserve the existing `syncdelay` knob as an explicit override.
- No new write path; only modulates wake cadence in the checkpoint thread.

## 3. Non-goals

- Changing the WiredTiger internal checkpoint contract.
- Per-collection or per-namespace checkpoint cadence.
- Eliminating shutdown / first-stable / triggered-checkpoint paths
  (`triggerFirstStableCheckpoint`, `pauseCheckpointThread`).
- Replication / oplog truncation logic.

## 4. Knob surface (new server parameters)

Added under the existing `storage_parameters.idl` `storage` group:

| Parameter | Type | Default | Bounds | `set_at` |
|---|---|---|---|---|
| `checkpointMinIntervalSecs` | `AtomicWord<int32_t>` | 5 | `[1, 3600]` | startup, runtime |
| `checkpointMaxIntervalSecs` | `AtomicWord<int32_t>` | 300 | `[1, 3600]` | startup, runtime |
| `checkpointOplogVolumeBytes` | `AtomicWord<int64_t>` | 16 * 1024 * 1024 | `[0, 1 << 32]` | startup, runtime |

Validators enforce `min <= max` at parameter-set time; mismatched values are
rejected with `ErrorCodes::BadValue` so we never produce a degenerate window.
`checkpointOplogVolumeBytes == 0` disables volume-based triggering and falls
back to the pre-existing `syncdelay`-only behaviour (compat mode).

Interaction with `syncdelay`: if the operator has set `syncdelay` to any value
other than its sentinel `-1.0`, that value wins and the volume gate is bypassed.
This is the explicit override contract — users with an existing tuned
`syncdelay` see no behaviour change.

## 5. Algorithm

```
loop in Checkpointer::run():
  oplogBaselineBytes := storageEngine->oplogBytesWritten()
  sleepBudget       := checkpointMaxIntervalSecs
  pollInterval      := checkpointMinIntervalSecs

  while sleepBudget > 0 and not shuttingDown and not triggered:
    wait(pollInterval) on _sleepCV
    sleepBudget -= pollInterval

    delta := storageEngine->oplogBytesWritten() - oplogBaselineBytes
    if delta >= checkpointOplogVolumeBytes:
      break   // volume gate fired

  storageEngine->checkpoint()
```

State stays on the stack; no new mutex. The condition variable wait already
exists. `oplogBytesWritten()` is a cheap atomic counter we already maintain
in the oplog truncate-marker code path (`OplogTruncateMarkers::_currBytes`).

### 5.1 Why poll instead of "wake on oplog write"

A signal-on-every-write path adds cost to the hot insert path. Polling at
`checkpointMinIntervalSecs` (default 5s) keeps the existing decoupled-thread
shape. The maximum extra checkpoint latency we incur is one poll interval.

### 5.2 Interaction with explicit triggers

`triggerFirstStableCheckpoint` and `_triggerCheckpoint` short-circuit the loop
exactly as today. Shutdown still takes a final checkpoint. `pauseCheckpointThread`
failpoint still works since the pause point is unchanged.

## 6. Backout / revert plan

`checkpointOplogVolumeBytes = 0` reverts to legacy behaviour at runtime, no
restart required. The SERVER-114491 DSC-syncdelay workaround can be reverted
in a follow-up once this knob has soaked one release with telemetry showing
phylog/oplog write ratios in line with expectation (see grafana panel
attached on the ticket).

## 7. Risk analysis

| Risk | Mitigation |
|---|---|
| Recovery window grows unbounded under quiet workload | `checkpointMaxIntervalSecs` upper bound (default 5min, matches old syncdelay ceiling). |
| Volume threshold lets a write storm produce thrash | `checkpointMinIntervalSecs` floor; loop never re-enters checkpoint inside that window. |
| `oplogBytesWritten()` counter wraps or is reset | Counter is monotonic per process; reset only on restart. After restart the baseline is the post-recovery counter value — first checkpoint will fire by max-interval, not by delta. |
| Standalone (no oplog) | If `oplogBytesWritten()` returns 0 forever, volume gate never fires; checkpoint cadence collapses to `checkpointMaxIntervalSecs`. Acceptable — standalones have no replica recovery requirement. |
| `syncdelay` explicit override is bypassed | Detected by sentinel check (`syncdelay != -1.0`); legacy path wins. |
| Param set racing with thread wakeup | Both knobs are `AtomicWord`; read at top of each iteration. Worst case: one iteration uses the old value. |

## 8. Test plan

- **Unit:** `WiredTigerKVEngineTest` — covered by impl PR (not part of this commit).
- **e2e jstest:** `jstests/noPassthrough/wt_integration/checkpoint_oplog_volume.js`
  exercises the knob matrix:
  1. Defaults take a checkpoint after at most `checkpointMaxIntervalSecs`
     even with zero oplog writes.
  2. Driving `> checkpointOplogVolumeBytes` of inserts fires a checkpoint
     before `checkpointMaxIntervalSecs`.
  3. `checkpointMinIntervalSecs` is honoured — a write burst inside that
     window does not double-checkpoint.
  4. Setting `syncdelay` explicitly (sentinel cleared) overrides volume gate.
  5. Setting `checkpointOplogVolumeBytes = 0` falls back to legacy
     `syncdelay`-only cadence.
  6. Validator rejects `min > max` at runtime.
- **Soak / perf:** sys-perf low-throughput-DSC variant (off this PR);
  expect phylog/oplog ratio to converge near 1.0 over a day, matching the
  grafana panel on the ticket.

## 9. Observability

`db.serverStatus().wiredTiger.checkpoint` already exposes
`generation` / `most recent time` / `total time`. We add two scalar fields
that the existing checkpoint stats reporter can pick up cheaply:

- `oplogBytesAtLastCheckpoint` — value of `oplogBytesWritten()` at the
  moment the most recent checkpoint started.
- `triggerReason` — enum `{interval, volume, explicit, shutdown}`.

These are not part of the wire-format contract; they are
`serverStatus`-only diagnostic surfaces.

## 10. Rollout

1. Land knob with defaults set to `min=5`, `max=300`, `volume=16MB`.
2. One release soak with telemetry.
3. Revert SERVER-114491 workaround.
4. Consider bumping DSC default `max` toward 300s formally (today the
   override knob does it).
