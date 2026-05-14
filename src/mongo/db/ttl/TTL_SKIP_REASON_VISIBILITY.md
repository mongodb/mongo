# TTL skip-reason visibility (SERVER-88352)

## Problem

`TTLMonitor::_doTTLIndexDelete` swallows the missing-namespace case silently. At
`src/mongo/db/ttl/ttl_monitor.cpp:460-469`:

```cpp
auto nss = collectionCatalog->lookupNSSByUUID(opCtx, uuid);
if (!nss) {
    if (info.isClustered()) {
        ttlCollectionCache->deregisterTTLClusteredIndex(uuid);
    } else {
        ttlCollectionCache->deregisterTTLIndexByName(uuid, info.getIndexName());
    }
    return false;
}
```

The branch fires whenever a TTL pass races a drop, a `configShard` window (the case
described in the ticket — sharding filter metadata not yet attached), or any other source
of a stale `TTLCollectionCache` entry. No log line, no counter, no `currentOp` field.
Three causes converge on one indistinguishable silent skip. Operators investigating "why
didn't my TTL fire" have to read the source to know the case even exists.

`SERVER-92779` tracks the adjacent but distinct orphan-doc visibility gap on sharded
collections; the two should not be collapsed because their remediation paths differ
(orphans need range-deleter coordination; namespace-not-found needs catalog re-lookup or
sharding-metadata refresh).

## Proposed change

### 1. `serverStatus().metrics.ttl.skipReasons` histogram

Three `Counter64` metrics, registered next to the existing ones at
`ttl_monitor.cpp:131-134`:

```cpp
auto& ttlSkipNamespaceNotFound =
    *MetricBuilder<Counter64>{"ttl.skipReasons.namespaceNotFound"};
auto& ttlSkipOrphan  = *MetricBuilder<Counter64>{"ttl.skipReasons.orphan"};
auto& ttlSkipOther   = *MetricBuilder<Counter64>{"ttl.skipReasons.other"};
```

Increment `ttlSkipNamespaceNotFound` inside the `if (!nss)` branch before the early
`return false`. `orphan` is reserved for SERVER-92779 to wire in; `other` is the residual
catch-all (e.g. `isTemporaryReshardingCollection`, `canAcceptWritesFor` false) and should
stay near zero in steady state.

### 2. `currentOp` `ttl` sub-document

Add a `ttl` field to the `TTLMonitor` `currentOp` row carrying the same histogram plus a
`lastSkip: {uuid, reason, ts}` triple. The histogram comes free from the counters above
(read via `Counter64::get()`); `lastSkip` is a single `WithLock`-guarded struct on
`TTLMonitor` updated on each skip path.

### 3. Structured log on the silent branch

The current code emits no log when `!nss`. Add a `LOGV2_DEBUG(level=1, …,
"reason"_attr = "namespaceNotFound", "uuid"_attr = uuid)` matching the shape of the
existing `5400703` error log. Debug-level keeps prod logs quiet; ops can raise verbosity
on demand.

## Why this shape

SERVER-43194 set the pattern: when an internal decision is already being tracked but only
exposed at the end of a long pipeline, the cheap fix is to surface the existing counter
through the standard observability surfaces (`serverStatus`, `currentOp`, `explain`) rather
than re-architect the decision point. TTL already *makes* the skip decision per-UUID
per-pass — it just doesn't *report* it.

The histogram is cheap (three atomic increments), zero-allocation on the hot path, and
keeps the three failure modes structurally distinct so downstream automation (Cloud
metrics alerting, MMS surfacing) can fan out by reason without parsing logs.

## Code site

Single file, one branch:

- `src/mongo/db/ttl/ttl_monitor.cpp:460-469` — increment + `lastSkip` write + debug log.
- `src/mongo/db/ttl/ttl_monitor.cpp:131-134` — counter registration.
- `src/mongo/db/ttl/ttl_monitor.h` — `lastSkip` struct + mutex.

## Test

`jstests/noPassthrough/ttl/ttl_namespace_not_found_visibility.js` pre-registers the
expected counter + `currentOp` shape. It already passes against the unfixed server (the
new-counter assertions are skipped when the fields are absent). When the fix lands, those
guards flip to live assertions with no other change to the test.
