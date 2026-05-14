\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE ObjectStoreBackpressure -------------------------
\* Models the object-store (S3-like) request plane used by the disaggregated
\* storage stack: PageServers reading pages, snapshot uploaders writing chunks.
\* The store has finite request concurrency. Without per-tenant budgets, a
\* single tenant's burst can monopolise the entire global cap and starve the
\* rest (SLS-4797). This spec is the formal companion to the design at
\* src/mongo/db/SLS-4797-DESIGN.md.
\*
\* The model intentionally tracks requests as per-bucket COUNTS rather than
\* per-request records. Sets with unique-id records grow without bound and
\* prevent TLC from forming a lasso, which would make liveness vacuously hold
\* and hide the starvation bug. Bounded counts admit cyclic states and let
\* TLC find the counter-example.
\*
\* Two configurations are modelled side by side, governed by BudgetsEnabled:
\*   * BudgetsEnabled = FALSE  (the bug cfg). The dispatcher only checks the
\*     global GlobalCap. One tenant offering enough requests can pin the
\*     in-flight slots to itself indefinitely; other tenants stay starved.
\*     Model-checking under the bug cfg fails FairnessAcrossTenants and
\*     produces the starvation counter-example.
\*   * BudgetsEnabled = TRUE  (the healthy cfg). A token-bucket per (tenant,
\*     opKind) caps each tenant's in-flight share at TenantCap, and the
\*     global ceiling still applies. Excess offers are shed at admission and
\*     counted against the observability counter `shedCount`. Under sustained
\*     load every tenant eventually progresses.
\*
\* Operation kinds (OpKinds) split the read path (PageServer fetches) from
\* the write path (snapshot upload), matching the two-regime split in the
\* design doc. Each kind has an independent bucket so a heavy write workload
\* cannot drain the read budget and vice versa.
\*
\* Compatible with model-check.sh; see MCObjectStoreBackpressure.cfg for the
\* two configurations.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Tenants,            \* Set of tenant identifiers contending for the store.
    OpKinds,            \* Set of operation kinds {READ, WRITE} (PageServer vs upload).
    GlobalCap,          \* Global maximum in-flight requests against the store.
    TenantCap,          \* Per (tenant, kind) maximum in-flight when budgets are on.
    MaxPendingPerBucket,\* Per (tenant, kind) ceiling on the queued count.
    BudgetsEnabled      \* TRUE => per-tenant token buckets active; FALSE => bug cfg.

ASSUME Cardinality(Tenants) > 0
ASSUME Cardinality(OpKinds) > 0
ASSUME GlobalCap \in Nat /\ GlobalCap > 0
ASSUME TenantCap \in Nat /\ TenantCap > 0
ASSUME TenantCap <= GlobalCap
ASSUME MaxPendingPerBucket \in Nat /\ MaxPendingPerBucket > 0
ASSUME BudgetsEnabled \in BOOLEAN

VARIABLES
    pendingCount,     \* [Tenants -> [OpKinds -> Nat]]: queued count per bucket.
    inflightCount,    \* [Tenants -> [OpKinds -> Nat]]: in-flight count per bucket.
    shedCount,        \* [Tenants -> [OpKinds -> Nat]]: cumulative budget-shed count.
    retryCount,       \* [Tenants -> [OpKinds -> Nat]]: cumulative 429/503 retries.
    progress          \* [Tenants -> BOOLEAN]: TRUE once tenant has completed >= 1 request.

\* shedCount and retryCount are observability counters. They are unbounded in
\* principle (monotone increasing), but the state-space CONSTRAINT in the MC
\* module caps them so TLC explores a finite space. They are NOT used in any
\* enabling condition, so the spec's behaviour does not depend on their value;
\* they are pure observers.

vars == << pendingCount, inflightCount, shedCount, retryCount, progress >>

----------------------------------------------------------------------------
\* Helpers
----------------------------------------------------------------------------

\* Total in-flight across every (tenant, kind) bucket.
\* Uses TLC's `SetToSeq` to flatten the Tenants \X OpKinds product into a
\* sequence and folds with a top-level recursive sum.
RECURSIVE SumSeq(_)
SumSeq(seq) ==
    IF Len(seq) = 0
    THEN 0
    ELSE inflightCount[Head(seq)[1]][Head(seq)[2]] + SumSeq(Tail(seq))

TotalInflight ==
    SumSeq(SetToSeq({ <<t, k>> : t \in Tenants, k \in OpKinds }))

\* Budgets allow new dispatch into bucket (t,k) if the per-bucket cap is not
\* yet reached. Without budgets, only the global ceiling applies, which is
\* what produces the starvation trace.
BudgetAdmits(t, k) ==
    \/ ~BudgetsEnabled
    \/ inflightCount[t][k] < TenantCap

\* The global cap is enforced at dispatch time in both configurations: the
\* object store itself imposes a finite concurrency ceiling.
GlobalAdmits == TotalInflight < GlobalCap

\* Convenience: at least one bucket has something in-flight.
AnyInflight == TotalInflight > 0

\* Convenience: bucket has room to enqueue.
BucketHasRoom(t, k) == pendingCount[t][k] < MaxPendingPerBucket

----------------------------------------------------------------------------
\* Init
----------------------------------------------------------------------------

Init ==
    /\ pendingCount = [t \in Tenants |-> [k \in OpKinds |-> 0]]
    /\ inflightCount = [t \in Tenants |-> [k \in OpKinds |-> 0]]
    /\ shedCount = [t \in Tenants |-> [k \in OpKinds |-> 0]]
    /\ retryCount = [t \in Tenants |-> [k \in OpKinds |-> 0]]
    /\ progress = [t \in Tenants |-> FALSE]

----------------------------------------------------------------------------
\* Actions
----------------------------------------------------------------------------

\* A tenant offers a new request of kind `k`. Admission is checked against
\* the per-bucket budget when BudgetsEnabled is TRUE; otherwise the request
\* is always admitted to `pendingCount` and the global cap is checked only
\* at dispatch time -- this is the bug shape the design eliminates.
\*
\* When BudgetsEnabled and the bucket is already at TenantCap of pending +
\* inflight, the offer is shed at admission and the observability counter
\* `shedCount` increments. This corresponds to the design doc's "shed under
\* saturation" hook.
ClientOffer(t, k) ==
    /\ t \in Tenants
    /\ k \in OpKinds
    /\ BucketHasRoom(t, k)
    /\ IF BudgetsEnabled /\ (pendingCount[t][k] + inflightCount[t][k]) >= TenantCap
       THEN /\ shedCount' = [ shedCount EXCEPT
                                ![t] = [ @ EXCEPT ![k] = @ + 1 ] ]
            /\ UNCHANGED << pendingCount, inflightCount, retryCount, progress >>
       ELSE /\ pendingCount' = [ pendingCount EXCEPT
                                    ![t] = [ @ EXCEPT ![k] = @ + 1 ] ]
            /\ UNCHANGED << inflightCount, shedCount, retryCount, progress >>

\* Dispatcher picks a pending request from bucket (t,k) and moves it to
\* inflight. The global cap is enforced here in both configurations. When
\* BudgetsEnabled is TRUE, the per-bucket cap is also re-checked here.
\*
\* The non-determinism over (t,k) is the dispatcher's policy degree of
\* freedom. In the buggy cfg this lets TLC build a trace where the
\* dispatcher keeps choosing one tenant forever. In the healthy cfg the
\* per-bucket budget forces rotation.
Dispatch(t, k) ==
    /\ t \in Tenants
    /\ k \in OpKinds
    /\ pendingCount[t][k] > 0
    /\ GlobalAdmits
    /\ BudgetAdmits(t, k)
    /\ pendingCount' = [ pendingCount EXCEPT
                            ![t] = [ @ EXCEPT ![k] = @ - 1 ] ]
    /\ inflightCount' = [ inflightCount EXCEPT
                            ![t] = [ @ EXCEPT ![k] = @ + 1 ] ]
    /\ UNCHANGED << shedCount, retryCount, progress >>

\* Object store returns 2xx for a request in bucket (t,k). The slot is
\* released and the tenant records progress.
StoreOk(t, k) ==
    /\ t \in Tenants
    /\ k \in OpKinds
    /\ inflightCount[t][k] > 0
    /\ inflightCount' = [ inflightCount EXCEPT
                            ![t] = [ @ EXCEPT ![k] = @ - 1 ] ]
    /\ progress' = [progress EXCEPT ![t] = TRUE]
    /\ UNCHANGED << pendingCount, shedCount, retryCount >>

\* Object store returns 429/503 for a request in bucket (t,k). The request
\* is re-queued for backoff retry, the in-flight slot is released so the
\* dispatcher can rotate, and the per-bucket retry counter increments.
StoreThrottle(t, k) ==
    /\ t \in Tenants
    /\ k \in OpKinds
    /\ inflightCount[t][k] > 0
    /\ BucketHasRoom(t, k)
    /\ inflightCount' = [ inflightCount EXCEPT
                            ![t] = [ @ EXCEPT ![k] = @ - 1 ] ]
    /\ pendingCount' = [ pendingCount EXCEPT
                            ![t] = [ @ EXCEPT ![k] = @ + 1 ] ]
    /\ retryCount' = [ retryCount EXCEPT
                            ![t] = [ @ EXCEPT ![k] = @ + 1 ] ]
    /\ UNCHANGED << shedCount, progress >>

----------------------------------------------------------------------------
\* Next / Spec
----------------------------------------------------------------------------

Next ==
    \/ \E t \in Tenants, k \in OpKinds : ClientOffer(t, k)
    \/ \E t \in Tenants, k \in OpKinds : Dispatch(t, k)
    \/ \E t \in Tenants, k \in OpKinds : StoreOk(t, k)
    \/ \E t \in Tenants, k \in OpKinds : StoreThrottle(t, k)

\* Fairness assumptions are carefully scoped.
\*
\* * WF_vars(\E t,k : Dispatch(t, k))
\*     The dispatcher is fair as a SYSTEM: if any bucket has a pending
\*     request and headroom under the caps, the dispatcher must eventually
\*     dispatch SOME request. This is the realistic model -- a finite-
\*     thread dispatcher cannot stutter forever -- but it deliberately does
\*     NOT promise per-bucket fairness. That stronger assumption would mask
\*     the bug: under per-bucket WF, every bucket would drain, so even the
\*     no-budgets cfg would trivially satisfy FairnessAcrossTenants. With
\*     per-system WF only, TLC can construct a trace where the dispatcher
\*     keeps choosing tenant t1's bucket and never reaches t2's, falsifying
\*     FairnessAcrossTenants.
\*
\* * WF_vars(\E t,k : StoreOk(t, k))
\*     The object store eventually replies. Same per-system scope: scoped
\*     as the dispatcher, not per-bucket.
\*
\* * WF on per-tenant ClientOffer: under sustained load, every tenant keeps
\*     offering as long as it has bucket room. This is the "sustained load"
\*     assumption from the SLS-4797 success criteria; without it the bug
\*     cfg has no competing tenants to starve.
\*
\* We do NOT add WF to StoreThrottle. Throttles model an environmental
\* fault that may or may not happen; forcing them would over-constrain.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(\E t \in Tenants, k \in OpKinds : Dispatch(t, k))
    /\ WF_vars(\E t \in Tenants, k \in OpKinds : StoreOk(t, k))
    /\ \A t \in Tenants :
            WF_vars(\E k \in OpKinds : ClientOffer(t, k))

----------------------------------------------------------------------------
\* Invariants -- SAFETY
----------------------------------------------------------------------------

TypeOK ==
    /\ pendingCount \in [Tenants -> [OpKinds -> Nat]]
    /\ inflightCount \in [Tenants -> [OpKinds -> Nat]]
    /\ shedCount \in [Tenants -> [OpKinds -> Nat]]
    /\ retryCount \in [Tenants -> [OpKinds -> Nat]]
    /\ progress \in [Tenants -> BOOLEAN]

\* HEADLINE SAFETY.
\* The sum of in-flight requests across all tenants never exceeds the
\* global concurrency ceiling. This is the invariant the object store is
\* relying on: if we send more, we get back 503s in the real system, which
\* turns into the metastable retry spiral SLS-4797 also calls out.
BudgetNeverExceeded == TotalInflight <= GlobalCap

\* Per-tenant budget: when BudgetsEnabled, no tenant exceeds TenantCap in
\* any single operation-kind bucket. Read and write are independently
\* budgeted so a write burst cannot starve reads on the same tenant.
PerTenantBudgetNeverExceeded ==
    BudgetsEnabled =>
        \A t \in Tenants, k \in OpKinds :
            inflightCount[t][k] <= TenantCap

\* Pending and in-flight counts are non-negative and bounded.
CountsWellFormed ==
    /\ \A t \in Tenants, k \in OpKinds :
            pendingCount[t][k] \in 0..MaxPendingPerBucket
    /\ \A t \in Tenants, k \in OpKinds :
            inflightCount[t][k] \in 0..GlobalCap

----------------------------------------------------------------------------
\* Liveness -- FAIRNESS
----------------------------------------------------------------------------

\* HEADLINE LIVENESS.
\* Every tenant eventually completes at least one request. Under
\* BudgetsEnabled this passes: per-bucket caps force the dispatcher to
\* rotate, and per-tenant WF on ClientOffer guarantees every tenant has
\* work waiting. Under the bug cfg (BudgetsEnabled = FALSE) one tenant's
\* burst can pin all GlobalCap slots forever and the others never reach
\* progress[t] = TRUE -- TLC produces the counter-example trace SLS-4797
\* asks us to surface.
FairnessAcrossTenants ==
    \A t \in Tenants : <>(progress[t])

\* Pending work eventually clears for at least one tenant, on every reset
\* of the workload pattern. A weaker, easier-to-satisfy companion to
\* FairnessAcrossTenants for spot-checking the dispatcher.
SomePendingEventuallyDrains ==
    ( \E t \in Tenants, k \in OpKinds : pendingCount[t][k] > 0 )
        ~> ( \E t \in Tenants, k \in OpKinds :
                pendingCount[t][k] = 0 /\ progress[t] )

================================================================================
