# Multi-Threaded TTL Monitor — Design

Ticket: [SERVER-56195](https://jira.mongodb.org/browse/SERVER-56195)
Companion: [SERVER-56194](https://jira.mongodb.org/browse/SERVER-56194) (per-collection deletion limits)
Owner: Storage Execution
Status: Draft / proposal

## 1. Problem

`TTLMonitor` (`src/mongo/db/ttl/ttl_monitor.{h,cpp}`) is a single
`BackgroundJob`. Per pass, `_doTTLSubPass` iterates every TTL index
sequentially and invokes `_doTTLIndexDelete` inline on the monitor
thread (`ttl_monitor.cpp:433-446`). On large deployments with many TTL
indexes and high ingest the single deleter cannot keep up with writes,
TTL backlog grows unbounded, and customers have rolled their own
out-of-server reapers (see ticket history: 6 linked SF cases as of
2026-05-14).

The only existing pool inside the monitor is
`_metadataRefreshTaskExecutor` (`ttl_monitor.cpp:253-264`), capped by
`ttlMonitorMaxMetadataRecoveryThreads` — used solely for sharding
metadata refresh, never for the delete path.

## 2. Goals / Non-goals

Goals:

- Dispatch `_doTTLIndexDelete` work across a bounded worker pool so
  N TTL indexes make progress concurrently.
- Preserve the existing fairness invariant: each sub-pass visits every
  index that still has expired docs before re-visiting.
- Preserve per-index `ttlIndexDeleteTargetTimeMS` and
  `ttlIndexDeleteTargetDocs` semantics — concurrency multiplies
  aggregate throughput, not per-index budget.
- Keep WiredTiger write pressure bounded — knob-controlled cap on
  in-flight TTL deletes (storage engine cache eviction is the dominant
  cost; concurrency must not pathologically amplify it).
- Fully backward compatible: pool size = 1 reproduces today's behavior.

Non-goals:

- Cross-shard parallelism — each `mongod` parallelizes its own pass.
- Removing per-index batching limits (those are SERVER-56194's surface).
- Re-architecting the sharding metadata refresh path (already async).

## 3. Proposed architecture

### 3.1 Worker pool

Reuse the `ThreadPoolTaskExecutor` pattern already in `ttl_monitor.cpp`
for metadata recovery. Add a second `_deleteTaskExecutor` owned by
`TTLMonitor`, sized by a new server parameter
`ttlMonitorDeleteWorkers` (default `1` — preserves current behavior;
recommended `min(8, numCores)` once perf-validated).

Pool construction at startup mirrors `ttl_monitor.cpp:253-264`:

```cpp
ThreadPool::Options opts;
opts.poolName = "TTLDeleteWorkers";
opts.threadNamePrefix = "TTLDelete-";
opts.maxThreads = ttlMonitorDeleteWorkers.load();
opts.minThreads = 0;
_deleteTaskExecutor = executor::ThreadPoolTaskExecutor::create(
    std::make_unique<ThreadPool>(opts), executor::makeNetworkInterface("TTLDeleteNet"));
_deleteTaskExecutor->startup();
```

### 3.2 Sub-pass dispatch

`_doTTLSubPass` (`ttl_monitor.cpp:410-451`) changes from sequential
inner loop to fan-out + join. Per (uuid, info) pair, schedule a task
onto `_deleteTaskExecutor`. Each task creates its own `OperationContext`
from the monitor's `Client` factory (workers must NOT share the parent
opCtx — interruption + lock state are per-opCtx).

Pseudocode:

```cpp
struct ShardResult { UUID uuid; TTLCollectionCache::Info info; bool moreToDelete; };
std::vector<Future<ShardResult>> pending;
for (const auto& [uuid, infos] : work) {
  for (const auto& info : infos) {
    pending.push_back(_deleteTaskExecutor->scheduleWork(
      [this, uuid, info, at](const auto&) {
        ThreadClient tc("TTLDelete", getGlobalServiceContext()->getService());
        auto opCtx = tc->makeOperationContext();
        bool more = _doTTLIndexDelete(opCtx.get(), at, &ttlCollectionCache, uuid, info);
        return ShardResult{uuid, info, more};
      }));
  }
}
// Join — bounded by the sub-pass deadline.
for (auto& f : pending) {
  auto r = std::move(f).getNoThrow();
  if (r.isOK() && r.getValue().moreToDelete)
    moreWork[r.getValue().uuid].push_back(r.getValue().info);
}
```

Fairness is preserved: the outer `do { ... } while (!work.empty() &&
Seconds(timer.seconds()) < ttlMonitorSubPassTargetSecs)` survives
verbatim — every index is dispatched once per inner iteration before
any is re-visited.

### 3.3 New server parameter (add to `ttl.idl`)

```yaml
ttlMonitorDeleteWorkers:
    description:
        "Maximum number of worker threads the TTL monitor uses to perform
        per-index delete batches concurrently within a sub-pass. A value of 1
        reproduces the legacy single-threaded sub-pass."
    set_at: [startup, runtime]
    cpp_vartype: AtomicWord<int>
    cpp_varname: ttlMonitorDeleteWorkers
    default: 1
    validator:
        gte: 1
        lte: 64
    redact: false
```

`set_at: runtime` lets ops dial concurrency up/down without restart;
on update, the pool's `maxThreads` is reconfigured (`ThreadPool` supports
live resize) — analogous to `onUpdateTTLMonitorSleepSeconds`.

### 3.4 WT back-pressure

Add a soft brake: if the WT cache `bytes_dirty / cache_size` exceeds a
threshold (e.g. 0.20), the next sub-pass clamps effective workers to 1.
Implementation: poll `WiredTigerKVEngine::getStats()` at the top of
`_doTTLSubPass`. Threshold is a new parameter
`ttlMonitorDirtyCacheBackpressurePct` (default 20). This addresses the
ticket's explicit warning: "TTL deletes are very impactful on WT, so we
should be very careful to evaluate the performance consequences."

### 3.5 Per-collection serialization invariant

Two workers MUST NOT race on the same UUID. Today this is trivially
true (one thread). With a pool, enforce via an in-flight `stdx::set<UUID>`
guarded by a mutex — when dispatching, skip and re-queue any uuid whose
prior batch is still draining. This is cheap because the dispatch loop
already iterates UUIDs, not docs.

## 4. Observability

Extend the existing per-pass counters in `ttl_monitor.cpp`:

- `ttlPassDispatchedTasks` — counter, # of per-index tasks scheduled.
- `ttlPassQueuedTimeMicros` — sum of queue wait for dispatched tasks.
- `ttlPassWorkerStallEvents` — count of WT-backpressure clamps fired.
- `ttlMonitorDeleteWorkers` value mirrored into `serverStatus.metrics.ttl`.

These ride out via the existing `serverStatus` ttl section
(`ttl_monitor.cpp` `ttlPasses`, `ttlSubPasses` counters), exposed in the
perf jstest below.

## 5. Risk + roll-out

1. **Default `1`** — zero behavioral change at install time.
2. **Storage Execution dogfoods** a single dev cluster at workers=4.
3. **Sys-perf gates** on the new jstest (see §6) at workers ∈ {1, 4, 8};
   landing requires ≥1.8× throughput at workers=4 vs workers=1 on the
   many-indexes shape, with WT dirty-bytes ratio bounded.
4. **GA flip**: separate ticket once we have ≥2 release cycles of
   default-1 production telemetry.

## 6. Perf test plan

`jstests/noPassthrough/ttl_multi_threaded_perf.js` (skeleton landed
alongside this doc). Workload shapes:

- **Shape A — many small indexes**: 64 collections × 1 TTL index each,
  100k docs per collection, 30s expiry. Measures fan-out throughput.
- **Shape B — one fat index**: 1 collection × 1 TTL index, 10M docs,
  60s expiry. Confirms single-index workloads are not regressed.
- **Shape C — adversarial WT pressure**: Shape A while a concurrent
  bulk-insert workload runs at 50 MB/s. Confirms WT backpressure clamp
  engages.

Metric: docs deleted per second over a 60s steady-state window,
measured via the existing `ttl.deletedDocuments` `serverStatus` counter
(see `getTTLDeletedDocuments_forTest()` accessor). Workers ∈ {1, 2, 4, 8}.

Pass criterion: at workers=4 vs workers=1, Shape A ≥ 1.8×, Shape B
within ±10% (no regression), Shape C dirty-bytes stays under
configured backpressure threshold for ≥90% of the run.

## 7. Open questions

- Should the pool be shared with `_metadataRefreshTaskExecutor`?
  Probably not — different latency profile, separate failure modes,
  and metadata recovery is bursty while deletes are steady. Keep two
  pools.
- Resharding interaction (`ttl_resharding_collection.js`): the in-flight
  UUID set (§3.5) handles this; verify under chaos in dedicated
  follow-up before GA.
- Should `ttlMonitorBatchDeletes=false` (legacy mode) skip the pool
  entirely? Yes — legacy mode is doc-by-doc and re-introducing
  parallelism there would change semantics; dispatch on legacy mode
  short-circuits to inline call.
