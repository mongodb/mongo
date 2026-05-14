# SERVER-58026 — Omitted FTDC Sections Audit and Stabilization

**Ticket:** [SERVER-58026](https://jira.mongodb.org/browse/SERVER-58026) — *Omitted
FTDC sections cause frequent schema changes that limit FTDC retention.*

**Status:** Backlog (reporter: Bruce Lucas; assigned team: Server Security).
**Audit scope:** classify every FTDC sample subtree that can disappear from
`serverStatus`-style FTDC chunks at runtime, and propose section-level
stabilization rules so the metric chunk preserves a stable schema across
samples.

## 1. Why omissions hurt FTDC retention

FTDC stores each sample as a delta against the previous sample's schema. When a
top-level (or nested) section appears in sample *n* but disappears in sample
*n+1* (or vice versa), the metric chunk's reference document changes shape, the
chunk closes, a new chunk opens, and compression efficiency drops sharply.
Bruce Lucas reported retention collapsing from ~1 week to <2 days in a real
deployment because a small set of `serverStatus` subsections were dropping in
and out of FTDC chunks during steady-state operation. The data we lose
is exactly the data we want during the rare events FTDC exists to
investigate.

## 2. Catalog of currently omittable sections (mongod path)

The FTDC controller calls `serverStatus` via `FTDCServerStatusCommandCollector`
(`src/mongo/db/ftdc/ftdc_server.cpp`), which is wired through the standard
`ServerStatusSection` registry. Each section below is observably absent from
FTDC samples on some live deployments.

| # | FTDC path | Producer | Omission trigger | Frequency class |
|---|---|---|---|---|
| 1 | `serverStatus.wiredTiger` | `WiredTigerServerStatusSection::generateSection` → `WiredTigerUtil::collectConnectionStatistics` | `engine.tryGetStatsCollectionPermit()` returns `boost::none` (rollback-to-stable, checkpoint contention, shutdown/startup window). Returns `false`; no fields appended. | Sustained — minutes at a time during background WT activity |
| 2 | `serverStatus.wiredTiger.historyStorageStats` | `WiredTigerServerStatusSection::generateSection` → `WiredTigerUtil::historyStoreStatistics` | `engine.isEphemeral()` short-circuit OR no stats-collection permit. Subsection builder still opens, leaving an empty doc — its *shape* is variable when called against in-memory storage. | Steady-state on inMemory builds |
| 3 | `serverStatus.oplog` | `FTDCServerStatusCommandCollector::collect` (`commandBuilder.append("oplog", true)`) | Set to `false` during shutdown via the `_serverShuttingDown` flag. The flag flips on the first `ShutdownError` and *back* to `false` on a non-shutdown error — so a rollback-to-stable that surfaces a transient error temporarily drops `oplog`. | Sporadic — once per stepdown / rollback transient |
| 4 | `local.oplog.rs.stats` | `FTDCSimpleInternalCommandCollector` via `getCollectionSpecs` | The replicated-storage filter only registers this collector on data-storing replica-set members. On a standalone-promoted-to-replset transition the section appears mid-stream. Also vulnerable to `$collStats` `waitForLock=false` returning early. | Once per topology transition |
| 5 | `serverStatus.oplogTruncation` | `OplogTruncateMarkersServerStatusSection::generateSection` (`oplog_cap_maintainer_thread.cpp`) | Three nested conditionals: (a) oplog collection lookup returns null in read-only mode, (b) `LocalOplogInfo::get(opCtx)->getTruncateMarkers()` returns null, (c) the `oplogMinRetentionHours` storage parameter is unset. Each conditional changes the subsection's field set. | High — the per-section field shape mutates whenever truncate-markers creation method transitions (`InProgress` → `Sampling` / `Scanning`) |
| 6 | `serverStatus.encryptionAtRest` | Enterprise-only encryption status section | Section is registered only when an encrypted storage engine is bound; on a node that flips encryption mode (rare but observed during key rotation) the section appears/disappears. | Once per key-rotation cycle |
| 7 | `config.transactions.stats` / `config.image_collection.stats` | `getCollectionSpecs` predicate on `shouldUseReplicatedFastCount()` / `supportsFindAndModifyImageCollection()` | Driven by replicated-storage feature flags. Field set changes when persistence provider capabilities advertise differently. | Once per upgrade FCV bump |
| 8 | `serverStatus.transactions.lastCommittedTransactions` | Already excluded by `FTDCServerStatusCommandCollector` (`includeLastCommitted=false`) | *Already stabilized — listed here as the canonical precedent* | n/a |
| 9 | `serverStatus.metrics.apiVersions` | Already excluded by `FTDCServerStatusCommandCollector` (`metrics.apiVersions=false`) | *Already stabilized — listed here as the canonical precedent* | n/a |
| 10 | `serverStatus.sharding` / `serverStatus.timing` / `serverStatus.defaultRWConcern` | Already excluded by `FTDCServerStatusCommandCollector` | *Already stabilized — listed here as the canonical precedent* | n/a |

Rows 8-10 are the stabilization template the reporter implicitly asks for:
sections whose schema is hostile to delta encoding get suppressed at the
collector boundary, not inside the producer. Rows 1-7 are the remediation
queue.

## 3. Stabilization proposal

The fix has three components, ordered by blast radius:

### 3.1 Producer-side: emit a stable schema even on the unavailable path

For sections whose contents legitimately depend on transient state
(`wiredTiger`, `historyStorageStats`, `oplogTruncation`), the section's
`generateSection` should always emit *the same set of top-level fields*, with
sentinel values when the underlying data is unavailable. Example for
`wiredTiger`:

```cpp
bool ok = WiredTigerUtil::collectConnectionStatistics(*engine, bob);
if (!ok) {
    // Preserve schema: emit the same shape with sentinel values.
    bob.append("statisticsUnavailable", true);
    bob.append("permitAcquired", false);
}
```

This is the same pattern the existing `appendNumber("totalTimeProcessingMicros",
-1)` line in `OplogTruncateMarkersServerStatusSection` uses for the
`InProgress` case (lines 110-112) — generalize it across producers.

### 3.2 Collector-side: lengthen the sample timeout

The reporter's hypothesis is that the per-sample lock timeout is too short.
The current default lives in `FTDCConfig::kSampleTimeoutMillisDefault` and
is plumbed through `ftdcStartupParams.sampleTimeoutMillis`. A modest
increase (e.g., from the current default to ~750 ms) widens the window during
which producers can acquire the permits/locks they need without changing
their schemas. The startup parameter is already runtime-tunable
(`setParameter sampleTimeoutMillis`), so the change is one-line.

### 3.3 Schema-shape contract: an FTDC-stable assertion in tests

A jstest pinning the audit (Section 5 below) asserts that every section
catalogued in this audit either (a) appears in FTDC with a stable field set
across N consecutive samples, or (b) is *deliberately omitted* via the
collector's known exclusion list (`sharding`, `timing`, `defaultRWConcern`,
`metrics.apiVersions`, `transactions.lastCommittedTransactions`). Anything
else is a regression.

## 4. Rollout plan

1. Land producer-side stabilization for row #1 (`wiredTiger`) — highest
   blast radius, smallest diff.
2. Land jstest from Section 5 to pin the audit; it should pass against
   master once #1 lands.
3. Iterate rows #2-#7 in subsequent commits; each gated by extending the
   jstest's expected-section set.
4. Backport candidates: the reporter requested `v5.0` and `v4.4`; producer
   sentinel changes are safe to backport; `sampleTimeoutMillis` default
   bump is master-only.

## 5. Jstest pinning the audit

See `jstests/noPassthrough/ftdc/ftdc_omitted_sections_audit.js`. The test:

1. Boots a standard `MongoRunner.runMongod()`.
2. Calls `verifyGetDiagnosticData` to harvest an FTDC sample.
3. Walks the audit catalog above and asserts:
   - sections expected-present (rows 1-7, where the producer is registered)
     appear with a non-empty top-level field set;
   - sections expected-absent (rows 8-10) are *not* present;
   - the field set of `serverStatus.oplogTruncation` (a row-5 instance)
     contains the stable keys `totalTimeTruncatingMicros`, `truncateCount`,
     `interruptCount` regardless of truncate-markers state.
4. Collects two additional samples ~1s apart and asserts the *set of
   top-level keys* under `serverStatus.wiredTiger` is identical across
   samples (the load-bearing FTDC retention invariant).

The test is a behavioural pin on this audit document. Any future change
to a producer that flips field-set membership across samples will fail it,
forcing the contributor to either preserve the schema or extend the audit.

## 6. References

- Reporter's original case study: SERVER-58026 description (1-week retention
  collapse to <2 days).
- Precedent stabilization commits: the existing collector-side exclusion
  block in `FTDCServerStatusCommandCollector::collect` (`ftdc_server.cpp`
  lines 297-322).
- Related dependency (already closed): SERVER-70031 *Ensure WT is open when
  generating WiredTiger statistics.* — partial mitigation of row #1.
