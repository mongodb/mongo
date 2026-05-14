# SERVER-126178: Unify connPoolStats command and FTDC collector

## Status

Proposal. Targets `Networking & Observability` sprint queue. Owner: Joseph
Prince. References:

- Command body: `src/mongo/db/commands/conn_pool_stats.cpp` (`PoolStats::run`)
- FTDC body: `src/mongo/db/ftdc/networking_collectors.cpp` (`ConnPoolStatsCollector::collect`)
- Adjacent fix that exposed this divergence:
  [SERVER-125516](https://jira.mongodb.org/browse/SERVER-125516) /
  [SERVER-126186](https://jira.mongodb.org/browse/SERVER-126186) — search task
  executor stats had to land in `serverStatus` rather than `connPoolStats`
  because FTDC's `connPoolStats` collector did not pick them up.

## Problem

The two implementations have drifted. They share `ConnectionPoolStats`,
`globalConnPool`, and `Grid::getExecutorPool()`, but diverge in five concrete
ways. The drift is silent: each path compiles, each emits BSON, neither
references the other, and there is no test that pins them as equivalent
surfaces.

### Divergence table

| Field / source                                                             | `connPoolStats` command | FTDC `ConnPoolStatsCollector` | Effect |
| -------------------------------------------------------------------------- | ----------------------- | ----------------------------- | ------ |
| `globalConnPool.appendConnectionStats(&stats)`                             | yes                     | yes                           | parity |
| `numClientConnections` / `numAScopedConnections`                           | top-level on `result`   | top-level on FTDC builder     | parity (different builders) |
| `ReplicationCoordinator::appendConnectionStats` (replSet only)             | yes                     | **missing**                   | FTDC silently omits replication-pool counters on replSet nodes |
| `Grid::getExecutorPool()->appendConnectionStats`                           | yes (null-checked)      | yes (**not** null-checked)    | FTDC dereferences whatever `getExecutorPool()` returns; brittle if a future grid initialization order leaves it null |
| `customConnPoolStatsFn` (e.g. search task executors, mongot pool, etc.)    | yes                     | yes                           | parity for the closure path |
| `stats.appendToBSON(result)` vs `stats.appendToBSON(builder, forFTDC=true)`| `forFTDC=false`         | `forFTDC=true`                | intentional schema fork — FTDC drops `hosts`, keeps aggregates; pinned by existing `ftdc_connection_pool.js` |
| `ReplicaSetMonitorManager::get()->report(&result)`                         | `forFTDC=false`         | `forFTDC=true`                | intentional, same reason |
| `appendDiagnosticInfo(builder, "mongot", …)`                               | **missing**             | yes                           | mongot task-executor diagnostics emit to FTDC but **not** to the command |
| `appendDiagnosticInfo(builder, "searchIndex", …)`                          | **missing**             | yes                           | same — searchIndex diagnostics asymmetric |
| `ScopedAdmissionPriority(kExempt)`                                         | yes (command priority)  | n/a (FTDC runs out of band)   | not load-bearing for parity |

The two material divergences are: **(A)** the command reports
`ReplicationCoordinator::appendConnectionStats` and FTDC does not, and **(B)**
FTDC reports `mongot` / `searchIndex` `appendDiagnosticInfo` subsections and
the command does not. (A) is the bug that SERVER-126186 routed around by
moving search-task-executor stats to `serverStatus`. (B) is the reverse-shape
of the same gap.

## Proposed unification

Introduce a single helper, `appendConnPoolStatsCommon`, in
`src/mongo/db/ftdc/networking_collectors.{h,cpp}` (header re-exposed; no new
public package). Signature:

```cpp
void appendConnPoolStatsCommon(OperationContext* opCtx,
                               BSONObjBuilder& builder,
                               bool forFTDC);
```

Body: every line that is in **both** implementations today, plus the two
asymmetric blocks gated behind explicit branches so both callers can opt in:

1. `globalConnPool.appendConnectionStats(&stats)`
2. `numClientConnections` / `numAScopedConnections` (top-level)
3. `ReplicationCoordinator::appendConnectionStats` — emit on both paths
4. `Grid::getExecutorPool()->appendConnectionStats` — null-checked on both
   paths (use the command's existing guard)
5. `customConnPoolStatsFn`
6. `stats.appendToBSON(builder, forFTDC)` — `forFTDC` controls the
   `hosts` / aggregates fork; keeps existing FTDC schema invariant
7. `ReplicaSetMonitorManager::report(&builder, forFTDC)`
8. `appendDiagnosticInfo("mongot")` and `appendDiagnosticInfo("searchIndex")`
   — emit on both paths

After unification:

- `PoolStats::run` calls `appendConnPoolStatsCommon(opCtx, result, false)`
  and additionally retains the `ScopedAdmissionPriority(kExempt)` wrapper
  that is command-specific.
- `ConnPoolStatsCollector::collect` calls
  `appendConnPoolStatsCommon(opCtx, builder, true)`.

Net effect at the wire: `connPoolStats` command grows `mongot` /
`searchIndex` subsections it was previously missing (additive); FTDC grows
the replication-pool counters it was previously missing (additive). No field
is removed from either surface. The `forFTDC=true` schema fork in
`stats.appendToBSON` and `ReplicaSetMonitorManager::report` is preserved, so
existing FTDC consumers see no schema break.

## Rollback

The unification is a refactor over identical helper calls; rollback is
`git revert` of the single commit that introduces `appendConnPoolStatsCommon`
plus restoration of the two original function bodies. No data migration, no
catalog impact, no on-disk format change.

## Test plan

Two layers.

1. **Existing tests must continue to pass**:
   `jstests/sharding/conn_pool_stats.js` (command-side schema) and
   `jstests/noPassthrough/ftdc/ftdc_connection_pool.js` (FTDC-side schema,
   already pins the `hosts`-absent fork — see line 35 of that file).

2. **New parity test**:
   `jstests/noPassthrough/ftdc/ftdc_conn_pool_stats_parity.js` (added with
   this design). Runs `connPoolStats` on a mongos, fetches the FTDC
   `connPoolStats` subobject via `getDiagnosticData`, and asserts that the
   set of top-level keys in each surface is equal modulo a known
   intentional-fork list:

   - `hosts` (command only — FTDC drops by design when `forFTDC=true`)
   - `ok` / `$clusterTime` / `operationTime` (command envelope)

   The test does **not** assert value equality — `numCreated` and friends
   are point-in-time samples and will differ between the two reads — only
   key-set equality. Key-set equality is what the bug is about.

   The parity test also asserts that after this change, both surfaces
   contain `mongot` and `searchIndex` subsections (additive parity), and
   that both contain `pools` and `replicaSetMonitor` (existing parity that
   should not regress).

## Non-goals

- Changing the FTDC schema for any field other than the two additive
  subsections above (`mongot` / `searchIndex`) and the additive
  replication-pool counters.
- Touching `NetworkInterfaceStatsCollector` in the same file — it has no
  command-side twin and is not part of this divergence.
- Adding the `ScopedAdmissionPriority` priority wrapper to the FTDC path —
  FTDC's controller already gates collection cadence and operates on its
  own threads; admission priority is not a shared concern.
