# Query-Memory Load Shedding

## Purpose

Load shedding is a proactive, process-wide defense against out-of-memory (OOM) crashes caused by
memory-hungry queries. When the server's resident memory (RSS) climbs under load, the server begins
**probabilistically aborting** the operations most responsible for the pressure — larger,
longer-running, memory-tracked read operations — so the process stays alive and serves the rest of
its workload instead of being OOM-killed by the kernel. The response begins gently at a configurable
RSS low-water mark and escalates as pressure rises toward a high-water mark; this graduated response
sheds only as much as the pressure warrants, and the per-operation random roll spreads those aborts
out rather than shedding similarly-sized operations all at once.

The feature is **off by default** (`queryMemoryLoadSheddingLowMarkPercent = -1`) and ships dark.
Nothing runs — no monitor thread, and the hot-path check is a single relaxed atomic load — until an
operator (or an external policy such as mongotune) enables it at runtime.

## Signal vs. reaction

The design deliberately separates the _signal_ (how much memory pressure exists) from the _reaction_
(which operations to abort):

- **Signal**: a background `PeriodicRunner` job (`queryMemoryRssMonitor`) samples process RSS via
  `ProcessInfo::getResidentSize()` every `queryMemoryRssMonitorIntervalMillis` and publishes it to a
  `ServiceContext` decoration (`QueryMemoryLoadSheddingState`). The process memory limit
  (`ProcessInfo::getMemSizeBytes()`, cgroup-aware) is captured once at `ServiceContext` creation.
- **Reaction**: at every check-for-interrupt point, each memory-tracked operation independently
  rolls a probability derived from the current pressure and its own footprint, and aborts itself if
  the roll succeeds.

There is **no global reservation pool or shared accounting** — an earlier design had one and it was
removed. Each operation reads shared state (RSS, knobs) but decides alone, so there is no cross-core
contention beyond the monitor's occasional single-word store.

## The shed decision

The per-check probability is a pure function (`query_memory_load_shedding_detail::shedProbability`,
unit-tested in isolation):

```math
\begin{aligned}
\text{pressure} &= \mathrm{clamp}\left(\frac{\text{RSS\%} - \text{lowMark}}{\text{highMark} - \text{lowMark}},\; 0,\; 1\right), \qquad \text{RSS\%} = 100 \cdot \frac{\text{RSS}}{\text{memLimit}} \\[4pt]
\text{size} &= \frac{\text{opTrackedBytes}}{\text{sizeReferenceBytes}} \\[4pt]
\text{hazard} &= \ln(2) \cdot \text{size} \cdot \frac{\text{pressure}}{1 - \text{pressure}} \\[4pt]
p &= 1 - e^{-\,\text{hazard}\,\cdot\,\Delta t} = 1 - 2^{-\,\text{size}\,\cdot\,\frac{\text{pressure}}{1-\text{pressure}}\,\cdot\,\Delta t}
\end{aligned}
```

```
pressure = clamp((RSS/memLimit*100 - lowMarkPercent) / (highMarkPercent - lowMarkPercent), 0, 1)
size     = opTrackedBytes / sizeReferenceBytes           # reference-size units, unbounded above
hazard   = ln(2) * size * pressure / (1 - pressure)      # per-second abort rate; diverges at high mark
p        = 1 - exp(-hazard * dt)                         # probability for this check
```

with two hard endpoints: `p = 0` at/below the low mark (`pressure <= 0`) and `p = 1` at/above the
high mark (`pressure >= 1`), for every operation regardless of size.

- **`pressure`** ramps the shed probability from **0% at the low-water mark to 100% at the
  high-water mark**. The hazard uses the _odds_ of pressure (`pressure/(1-pressure)`) so it diverges
  as pressure approaches the high mark — that is what pins the probability to 1 there without
  needing a separate rate parameter.
- **`size`** biases shedding toward the operations actually consuming memory. It is _not_ clamped at
  1: an operation at twice the reference size is shed twice as fast, so the largest operations go
  first. Operations well below the reference are effectively never shed until pressure nears the
  high mark. There is intentionally **no floor** forcing small operations to be shed.
- **`hazard`** is the per-second shedding _rate_ (in the survival-analysis sense), not a
  probability: shedding is modeled as a memoryless Poisson process, so `p = 1 - exp(-hazard * dt)`
  is the chance it fires within the elapsed interval and `1 / hazard` is the mean time-to-shed.
  Being a rate is what lets the decision compose across irregular checkpoints and stay memoryless.
- **`dt`** is the wall-clock time since this operation's previous evaluation. Driving the hazard
  through `dt` makes shedding **time-based, not check-count-based**: an operation that checks for
  interrupt thousands of times per second is not shed any faster than one that checks rarely. It is
  the key to fairness across operations and query shapes. This property holds for inter-check
  intervals up to the 1 s `dt` cap (see "Limitations"); the sub-millisecond throttle preserves it at
  the high-frequency end by accruing skipped time rather than dropping it.

**The `ln(2)` factor makes `sizeReferenceBytes` explainable as a half-life.** With it, a
reference-size operation at mid-pressure (RSS halfway between the marks, odds = 1) has hazard
`ln(2)`, i.e. a **50% chance of being shed per second — a one-second shed half-life**. Size scales
that half-life inversely: a 2× operation has a ½-second half-life, a ½× operation a 2-second
half-life. So you can reason about the knob directly ("how big must an operation be to have a
one-second half-life at mid-pressure?") instead of about an abstract rate constant.

### Per-check probability by size and pressure

With the default `sizeReferenceBytes = 32 MiB` and one second of accumulated exposure (`dt = 1s`),
`p_check` as a function of `pressure` (0 = low mark, 1 = high mark):

| pressure | 1KB   | 1MB   | 32MB      | 256MB | 1GB   |
| -------- | ----- | ----- | --------- | ----- | ----- |
| 0.0      | 0.000 | 0.000 | 0.000     | 0.000 | 0.000 |
| 0.1      | 0.000 | 0.002 | 0.074     | 0.460 | 0.915 |
| 0.25     | 0.000 | 0.007 | 0.206     | 0.843 | 0.999 |
| 0.5      | 0.000 | 0.021 | **0.500** | 0.996 | 1.000 |
| 0.75     | 0.000 | 0.063 | 0.875     | 1.000 | 1.000 |
| 0.9      | 0.000 | 0.177 | 0.998     | 1.000 | 1.000 |
| 0.99     | 0.002 | 0.883 | 1.000     | 1.000 | 1.000 |
| 1.0      | 1.000 | 1.000 | 1.000     | 1.000 | 1.000 |

The bold cell is the calibration anchor: the 32MB reference op at mid-pressure is shed with exactly
50% probability per second. Larger operations (256MB, 1GB) are shed with high probability even just
above the low mark, while sub-reference operations (1KB, 1MB) are spared until pressure climbs
toward the high mark — and at the high mark everything is shed.

### Effect of dt

A single evaluation covers the interval `dt` since the operation's previous check, so its per-check
probability scales with `dt`. For the reference-size op (32MB) at mid-pressure (the 0.5 row above,
or, equivalently, `hazard = ln(2)`):

| dt     | per-check p |
| ------ | ----------- |
| 1 ms   | 0.0007      |
| 10 ms  | 0.0069      |
| 50 ms  | 0.034       |
| 100 ms | 0.067       |
| 250 ms | 0.159       |
| 500 ms | 0.293       |
| 1 s    | 0.500       |

Crucially, the **cumulative** probability over a fixed exposure window is _independent_ of `dt` (and
therefore of how many times the operation happens to check). Split a window of length `T` into `n`
equal checks, each covering `dt = T/n`. By the memoryless property, every check is an independent
trial whose survival (not-shed) probability depends only on its own interval — so the survivals
multiply and the `n` cancels:

```math
\begin{aligned}
P(\text{survive one check}) &= e^{-\,\text{hazard}\cdot \Delta t} = e^{-\,\text{hazard}\cdot T/n} \\[4pt]
P(\text{survive all } n \text{ checks}) &= \left(e^{-\,\text{hazard}\cdot T/n}\right)^{n} = e^{-\,\text{hazard}\cdot (T/n)\cdot n} = e^{-\,\text{hazard}\cdot T} \\[4pt]
p_{\text{cumulative}}(T) &= 1 - e^{-\,\text{hazard}\cdot T} \qquad (n \text{ cancels})
\end{aligned}
```

```
survive one check = e^(-hazard * T/n)
survive all n     = (e^(-hazard * T/n))^n = e^(-hazard * (T/n) * n) = e^(-hazard * T)
p_cumulative(T)   = 1 - e^(-hazard * T)                # independent of n (and dt)
```

The result depends only on total elapsed time `T`, not on how it was chunked: the reference op
observed for one second is shed with probability 0.5 whether checked once (`dt = 1s`) or a thousand
times (`dt = 1ms`). That cancellation _is_ the memoryless property — a check carries no
per-operation state beyond `lastShedEvalTime`, so past checks never matter, only the elapsed
interval. It is the "time-based, not check-count-based" fairness property in action.

## Where the decision runs: the execution call sites

The decision is evaluated in `queryMemoryCheckLoadShedding(OperationContext*)`. When the roll
selects an operation it is aborted with `ErrorCodes::QueryMemoryLimitExceeded` (a `RetriableError` +
`SystemOverloadedError`, so well-behaved clients back off and retry later). The abort is made
**sticky** via `opCtx->markKilled(QueryMemoryLimitExceeded)` (the same mechanism as deadlines and
`killOp`) rather than a one-shot status: the operation stays killed, every later interrupt check
agrees, and the kill shows up in `currentOp`/kill metrics. Each call site invokes it only after the
operation's own interrupt check, so the operation is guaranteed not yet killed when the roll fires
and `operationsShed` counts each shed exactly once.

`queryMemoryCheckLoadShedding` is called directly from the query-execution interrupt/yield funnels —
**not** from a generic `OperationContext` hook — so the low-level `service_context` library stays
free of any query/memory-tracking dependency. Memory-tracked stages live in three execution contexts
with three interrupt funnels, and each is covered:

- **Classic aggregation** blocking stages (`$group`, `$sort`, …) accumulate memory _above_ the
  `PlanExecutor`, so they are evaluated where the pipeline checks for interrupt:
  `ExpressionContext::InterruptChecker::checkForInterruptSlow()` (throttled to once per 128
  documents), via an out-of-line `checkForLoadShedding()` shim in `expression_context.cpp`.
- **Classic `PlanStage` reads** (find blocking `SORT`, geoNear, `MERGE_SORT`, …) and **SBE with
  yielding enabled** both funnel through the shared `PlanYieldPolicy::yieldOrInterrupt()`
  (`plan_yield_policy.cpp`), where the call is made after the interrupt check on both the
  `INTERRUPT_ONLY` and `YIELD_AUTO` paths.
- **SBE with yielding disabled** checks interrupt in `CanInterrupt::checkForInterruptNoYield()`
  (`sbe/stages/stages.h`, throttled to once per 128), via the out-of-line
  `sbe::checkForLoadShedding()` shim in `sbe/stages/stages.cpp`.

The two header-inline funnels (ExpressionContext, SBE `CanInterrupt`) call out-of-line shims so
their widely-included headers don't pull in the load-shed library; the shim lives in each funnel's
own library, which depends on `query_memory_load_shedding`.

### Hot-path cost

These funnels are extremely hot, so the off cost and the enabled-but-healthy cost are both
engineered to be cheap:

1. When the feature is off — the default — `queryMemoryCheckLoadShedding` returns after a single
   relaxed atomic load (`loadSheddingEnabled()`), so each call site pays only that load at its
   already-throttled interrupt point.
2. When enabled, `queryMemoryCheckLoadShedding` short-circuits at/below the low water mark (the
   steady state) after a couple of relaxed atomic loads — the RSS sample compared against the
   precomputed low-mark threshold — plus one small per-operation store, still **before** reading the
   clock or touching the probability pipeline.
3. Above the low mark, a per-operation throttle (`kMinShedEvalInterval`, 1 ms) skips re-evaluation
   if too little wall-clock time has passed **without** updating the last-eval timestamp, so the
   elapsed time accrues into a later `dt`. This bounds the `exp()` and RNG work to ~once per ms per
   op while preserving the time-based semantics.

The random roll reuses the client's warm PRNG via `Client::getPrng().trueWithProbability(p)`.

## Operation eligibility

`queryMemoryCheckLoadShedding` sheds an operation only if all of these hold:

- The process is a **data-bearing** node, not a pure router (see "Limitations & future work").
- The feature is enabled (`lowMarkPercent >= 0`).
- The operation has an `OperationMemoryUsageTracker` (via `getIfExists`, which never creates one) —
  i.e. it is a memory-tracking read. Point queries, writes without tracked memory, and internal
  bookkeeping have no tracker and are never shed.
- The operation is marked **eligible** (see the next section).
- The operation has not taken a write-intent global lock (`wasGlobalLockTakenForWrite()`); see
  "Writes are exempt via the write-intent lock latch".
- `pressure > 0` and `size > 0` and the probabilistic roll fires.

### Eligibility is opt-in

Shedding is opt-in: an operation is a candidate only after a user-facing read command marks it, via
`markOperationQueryMemorySheddingEligible()` at the top of `run()`. The read commands opt in on both
the shard (mongod) and router (mongos) sides: **find**, **aggregate**, **getMore**, and **distinct**
(`count` is deliberately left out — its aggregation fallback is a single-group count with negligible
tracked memory, so it is never a candidate). Everything unmarked is never shed — replication,
migration, TTL, index builds, and internal reads that build executors directly (e.g. the resharding
pipelines).

Keying off the command needs no sharding-specific reasoning: shard-side execution of a user query
arrives as an `aggregate`/`find` command, the merging half of a sharded aggregation is an
`aggregate` command (with `$mergeCursors`), and a getMore is its own command. So each opts in on its
own opCtx — nothing is inferred from the connection or shard version, and nothing is stashed on the
cursor.

The **router** (`cluster_*`) commands mark eligible too, so query work on an embedded-router mongod
is sheddable (on a pure mongos the process gate below keeps shedding off, so the marks are no-ops
there). The one exception is a **write** aggregation (`$merge`/`$out`) on the router: the router
marks `cluster_aggregate` eligible only for read-only pipelines (`!aggHasWriteStage`), because a
router's writes are remote dispatch to shards that bypass the exemption latch — shedding one could
abort a partial cross-shard write. Covering that is deferred to router support (SERVER-131578).

An internal path that _should_ be sheddable can opt in explicitly: `analyzeShardKey`'s metrics
aggregations mark themselves eligible, since they are heavy diagnostic work for which a retryable
back-off under memory pressure is the right response.

The flag is a per-opCtx decoration (like the exemption latch), defaulting false.

### Writes are exempt via the write-intent lock latch

An operation that has begun a write must not be aborted mid-way, or shedding would surface a
`RetriableError` after a partial, non-idempotent write that a naive client retry could re-apply. We
detect this from the `Locker`: `queryMemoryCheckLoadShedding` skips any op for which
`wasGlobalLockTakenForWrite()` is true — i.e. it has ever taken the global lock in `IX`/`X` (write
intent). This is a **sticky, one-way signal**: the Locker's `_globalLockMode` bits are never
cleared, so once an op takes a write-intent lock it stays exempt for the rest of its life, and
surviving yields.

- Update/delete/findAndModify (including a whole `multi:true` statement) take a collection/global
  `IX` before writing, so they are exempt for the duration.
- `$merge`/`$out` take `IX` for the write phase; the upstream blocking `$group`/`$sort` runs before
  any write lock, so it stays sheddable.
- A read in a multi-document transaction that has already written is exempt too (the txn took `IX`
  earlier) — we won't abort a txn with uncommitted writes.

The exemption is distinct from eligibility: eligibility (opt-in, above) governs _whether_ an
operation can be shed at all, while the write-intent lock check protects an _eligible_ operation
once it has begun a write.

## Components

| File                                     | Role                                                                                                                                                                                                                        |
| ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `query_memory_load_shedding.h/.cpp`      | The decision (`queryMemoryCheckLoadShedding`), the pure `shedProbability`, the RSS monitor (`startQueryMemoryRssMonitor`), the `QueryMemoryLoadSheddingState` decoration, stats, and the on-update hook.                    |
| `query_memory_load_shedding.idl`         | Server parameters.                                                                                                                                                                                                          |
| `operation_memory_usage_tracker.{h,cpp}` | Per-operation tracked-memory total (`inUseTrackedMemoryBytes()`, `getIfExists()`, `lastShedEvalTime`) plus the eligibility flag: `markOperationQueryMemorySheddingEligible()` / `isOperationQueryMemorySheddingEligible()`. |
| `expression_context.{h,cpp}`             | Classic-agg call site: `InterruptChecker::checkForLoadShedding()` shim.                                                                                                                                                     |
| `plan_yield_policy.cpp`                  | Classic-PlanStage / yielding-SBE call site, in `yieldOrInterrupt()`.                                                                                                                                                        |
| `sbe/stages/stages.{h,cpp}`              | No-yield SBE call site: `sbe::checkForLoadShedding()` shim, called from `CanInterrupt::checkForInterruptNoYield()`.                                                                                                         |
| `query_memory_server_status_section.cpp` | `serverStatus().queryMemory`.                                                                                                                                                                                               |
| `mongod_main.cpp`                        | Calls `startQueryMemoryRssMonitor` at startup (data-bearing nodes only; a pure router never starts it).                                                                                                                     |

## Server parameters

All are runtime-settable unless noted.

| Parameter                                   | Default         | Meaning                                                                                    |
| ------------------------------------------- | --------------- | ------------------------------------------------------------------------------------------ |
| `queryMemoryLoadSheddingLowMarkPercent`     | `-1` (disabled) | RSS % of the memory limit at/below which nothing is shed. Enables the feature when `>= 0`. |
| `queryMemoryLoadSheddingHighMarkPercent`    | `85`            | RSS % at/above which every eligible op is shed (probability 1).                            |
| `queryMemoryLoadSheddingSizeReferenceBytes` | `32 MiB`        | Tracked-memory size treated as one size unit; larger ops are shed proportionally faster.   |
| `queryMemoryRssMonitorIntervalMillis`       | `100`           | RSS sampling interval (startup-only).                                                      |

## Observability

`serverStatus().queryMemory.loadShedding` (present only while enabled) reports: `currentUsageBytes`
(latest RSS), `lowMarkBytes`, `highMarkBytes`, `memLimitBytes`, and `operationsShed` (cumulative
count of shed operations).

For fleet-wide monitoring, `operationsShed` is also exported as an OpenTelemetry counter
(`mongodb.serverStatus.queryMemory.loadShedding.operationsShed`), which appears in `serverStatus` by
default and flows into the metrics ingestion/Grafana pipeline. The in-process atomic remains the
source of truth; the OTel counter is an export-only mirror (the opcounters pattern). RSS and the
memory limit are _not_ mirrored to OTel — process RSS is already `serverStatus().mem.resident`, and
the memory limit is available from `hostInfo` (`system.memLimitMB`) — so a dashboard can compute
RSS-vs-limit from those without a duplicate metric.

Each shed also emits a log line (id `13033300`) that identifies the shed operation — namespace,
redacted command, plan summary, and opId — alongside the pressure fields, rate-limited via a
`SeveritySuppressor` to one full line per second (the first at `Warning`, the rest at debug
severity).

## Limitations & future work

- **The lever only reclaims what killing a query frees.** The pressure signal is process RSS, so it
  sees all memory — but the only response is aborting query operations. An OOM dominated by memory
  not attributable to sheddable queries (e.g. WiredTiger cache) is relieved only indirectly, which
  is why the high-water mark must sit well below the ceiling: shedding needs lead time to turn
  aborted queries into freed RSS.
- **Only memory-tracked reads are eligible.** Writes are exempt (see "Exemption is a dynamic
  execution window"), and operations without an `OperationMemoryUsageTracker` (point queries, most
  non-query commands) are never shed. Targeting is also biased by _tracked_ bytes, so a query whose
  footprint is mostly untracked is under-weighted (still sheddable, just not preferred).
- **The memory limit is captured once at startup.** `memLimitBytes` is sampled from
  `ProcessInfo::getMemSizeBytes()` at `ServiceContext` creation and never refreshed; only RSS is
  re-sampled. After an in-place cgroup memory resize (e.g. a Kubernetes vertical resize) the
  pressure denominator is stale until the process restarts — over-shedding after a scale-up,
  under-shedding before a scale-down. This is a deliberate trade-off: mongod already treats the
  memory size as fixed at startup (e.g. for WiredTiger cache sizing), so refreshing it here alone
  would not make the process as a whole resize-aware. Restart the process to pick up a new limit.
- **The `dt` cap breaks time-based fairness beyond 1 s between checks.** `dt` is capped at 1 s so a
  long pause can't credit the current hazard over an interval during which pressure was likely
  different, and so a system-wide stall (lock convoy, slow storage) doesn't shed every op at once
  when execution resumes. The cost: an operation that only reaches a checkpoint less often than once
  per second is under-shed proportionally (e.g. a 5 s gap credits 1 s, so ~5x under-shed), so the
  "time-based, not check-count-based" property only holds up to the cap. In practice this is the
  safe direction and hits the case that matters least: an operation actively accumulating tracked
  memory checks far more often than once per second, while gaps over a second mean it is stalled and
  not growing — and it is re-evaluated at the full rate as soon as it resumes checking frequently.
- **Pure mongos and router writes are not covered (SERVER-131578).** Shedding runs only on
  data-bearing nodes (`loadSheddingSupportedOnThisProcess` excludes a process whose role is
  _exclusively_ router), so a **pure mongos** never sheds — its router query work, including
  memory-hungry merge pipelines, is a gap. An **embedded-router mongod** (shard + router role) does
  shed its router read work, since the `cluster_*` read commands opt in (see "Eligibility is
  opt-in"). What remains uncovered on any router is **write** aggregations: a router's
  `$merge`/`$out` and cluster writes are remote dispatch to shards that bypass the exemption latch,
  so they are left ineligible to avoid shedding a partial, non-idempotent cross-shard write.
  Covering pure mongos and router writes is deferred follow-up work.
- **Soft, probabilistic guard, not a hard ceiling.** It lowers the likelihood of a query-driven OOM
  rather than guaranteeing against one; the marks must leave enough headroom for shedding to react
  before RSS reaches the edge.
