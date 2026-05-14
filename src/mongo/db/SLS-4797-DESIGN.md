# SLS-4797: Object-store backpressure and per-tenant budgets

## Problem

The disaggregated storage stack issues requests to an S3-like object
store from two call sites with very different burst profiles: read-side
PageServer fetches (per page miss; latency-sensitive) and write-side
snapshot upload (large chunks; throughput-bound). The store enforces a
finite global concurrency ceiling. Today every caller competes for it
without coordination, so one tenant's burst monopolises the global
slots and starves the rest. Once the store starts returning 429/503 we
enter the metastable retry spiral the ticket names: retries crowd the
queue, queue latency exceeds the client timeout, timeouts enqueue more
retries, and the queue stays pinned long after the trigger cleared.

## Two regimes, two budgets

Read and write get independent admission control under a shared global
ceiling:

- `kReadBudget[tenant]` — maximum in-flight PageServer fetches per tenant.
- `kWriteBudget[tenant]` — maximum in-flight snapshot upload parts per
  tenant.
- `kGlobalCeiling` — dispatcher cap on total in-flight requests, sized
  below the store's published 503 threshold.

Invariant we owe the store: `Σ in-flight ≤ kGlobalCeiling`. Invariant we
owe operators: no tenant's burst can drive another tenant's read budget
to zero.

## Token-bucket per (tenant, operation-kind)

Each `(tenant, op_kind)` owns a token bucket sized to its budget.
Admission grabs a token; any terminal completion (2xx / 4xx / 5xx)
returns it. Retries do not consume a new token — they re-enqueue against
the same bucket. The dispatcher rotates over buckets that have both
pending work and a free token. Rotation is round-robin over the
non-empty bucket set, with weighted-fair degeneration when a tenant's
budget exceeds another's by more than 2×. Fairness is a property of the
data structure, not the dispatch policy.

The TLA+ companion at `src/mongo/tla_plus/Disagg/ObjectStoreBackpressure/`
models this admission discipline. Its bug configuration disables
per-tenant buckets and shows that the dispatcher's freedom to choose any
pending request is sufficient to starve other tenants — fairness has to
be enforced by the data, not promised by policy.

## Hooks

**Shedding under saturation.** When a bucket is at budget and a new
request arrives, the request is rejected at admission with a typed
`ObjectStoreBudgetExceeded` error. The caller chooses: PageServer falls
back to its in-memory miss handling (caller-visible latency); snapshot
upload defers to the next scheduling tick. We never enqueue above budget
— that is the metastability trap.

**Fairness across tenants.** The dispatcher rotates only over buckets
with positive pending and a free token. A tenant whose bucket is empty
contributes no slots to the rotation but also cannot block another
tenant's progress. The TLA+ liveness property `FairnessAcrossTenants`
codifies this: every tenant under sustained load eventually progresses.

**Observability counters.** Per (tenant, kind) we maintain `shedCount`,
`retryCount`, `inflightHWM`, `dispatchLatency_p99`. These back the SLO
targets in the ticket (`upload_throttle_rate < 1`,
`hot_range_materialization_lag_p99 < 1–2s`) and export via the existing
`disaggStorage_object_store_*` metric tree.

**Key/prefix sharding.** Out of scope here — tracked separately. The
budget machinery is correct regardless of sharding because budgets are
tenant-keyed; sharding only affects which physical prefix receives a
given request, not the admission gate.

## Acceptance

- `BudgetNeverExceeded` and `PerTenantBudgetNeverExceeded` proved in TLC
  under the healthy configuration.
- Bug cfg produces a `FairnessAcrossTenants` counter-example, attached
  to the PR.
- Integration test injects a heavy tenant burst and asserts lagging
  tenants' p99 materialization latency stays under 2s.
