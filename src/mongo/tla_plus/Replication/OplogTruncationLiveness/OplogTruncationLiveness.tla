\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

----------------------- MODULE OplogTruncationLiveness -----------------------
\* Models the oplog cap-maintainer thread's liveness obligation:
\*
\*   If MinRetentionHours > 0 and the oplog has grown beyond the retention cap,
\*   the cap maintainer thread MUST eventually shrink the oplog back to the
\*   retention cap.
\*
\* The production incident captured this property failing on six fleet
\* clusters: oplog kept growing while the cap-maintainer thread repeatedly
\* requested a truncation larger than the WiredTiger slow-truncate cache
\* budget allowed, hit a WriteConflictException, and retried the same
\* oversize window on the next tick (no size-based backoff). Per Jira
\* comments dated 2026-03-10 / 2026-03-18, every cycle attempted to remove
\* all excess oplog at once, so when one cycle was too big to fit in cache
\* the next cycle was too big as well, and the cluster never made progress.
\*
\* The spec models two variants gated by the BackoffOnConflict constant:
\*
\*   BackoffOnConflict = TRUE  (the fixed path):
\*       On WCE the maintainer drops the requested window to CacheBudget,
\*       analogous to the per-cycle cap from SERVER-122519 which switched
\*       the wake interval from 5 minutes to 30 seconds so each cycle's
\*       requested truncation stays well below the cache budget. The
\*       Liveness property holds.
\*
\*   BackoffOnConflict = FALSE (the bug path):
\*       The MaintainerTruncateConflict action is disabled (the prod
\*       maintainer had no working backoff path - on WCE it slept and
\*       retried the same oversize window with no mutation of state).
\*       Modeling this as a disabled action makes the stuck state a true
\*       terminal node with no outgoing edges, and TLC reports a liveness
\*       counter-example to Liveness: producer eventually drains, but the
\*       maintainer is left stuck with attemptSize > CacheBudget forever.
\*
\* To run TLC:
\*     cd src/mongo/tla_plus
\*     ./download-tlc.sh
\*     ./model-check.sh Replication/OplogTruncationLiveness
\*
\* The green config (MC.cfg) checks Liveness. The bug config (MC_bug.cfg)
\* drops the CONSTRAINT bound to make the violation manifest more quickly
\* and sets BackoffOnConflict = FALSE. See README.md for invocation notes.

EXTENDS Integers, Sequences, FiniteSets, TLC

\* ---------------------------------------------------------------------------
\* Constants
\* ---------------------------------------------------------------------------

\* Maximum number of oplog entries that can be appended in one Producer step.
\* "Bursty arrival" - matches the ticket's note that an affected cluster
\* "accumulated oplog at a phenomenal rate, possibly as high as 24.5GB per
\* hour for the first hour or two" (Jira comment, 2026-03-17).
CONSTANT MaxBurst

\* Per-cycle cache budget for slow truncate (WiredTiger ~3GB / 20% of cache
\* limit referenced by Keith Smith in the ticket). The maintainer can shrink
\* the oplog by at most CacheBudget entries in a single successful step.
CONSTANT CacheBudget

\* Number of oplog entries that constitute the retention window
\* (a model-checking proxy for MinRetentionHours: the maintainer is
\* obligated to keep at most RetentionCap entries on disk).
CONSTANT RetentionCap

\* Upper bound on log length for model-checking finiteness. The bug spec
\* will saturate at MaxOplogSize and stutter; TLC then reports the
\* liveness violation since the maintainer fails to shrink below
\* RetentionCap eventually.
CONSTANT MaxOplogSize

\* TRUE  : on WCE the maintainer halves the requested truncation window
\*         (the green / fixed model).
\* FALSE : on WCE the maintainer retries the same oversize window forever
\*         (the bug / production-incident model).
CONSTANT BackoffOnConflict

ASSUME MaxBurst       \in Nat \ {0}
ASSUME CacheBudget    \in Nat \ {0}
ASSUME RetentionCap   \in Nat \ {0}
ASSUME MaxOplogSize   \in Nat \ {0}
ASSUME RetentionCap < MaxOplogSize
ASSUME BackoffOnConflict \in BOOLEAN

\* ---------------------------------------------------------------------------
\* Variables
\* ---------------------------------------------------------------------------

\* oplogSize  - number of entries currently in the oplog. The model abstracts
\*              away byte size; one entry == one notional unit.
\* attemptSize - the size of the truncation the maintainer is going to ask
\*               for on its next attempt. Starts at RetentionCap (the
\*               natural "shrink-to-cap" request).
\* truncated  - cumulative count of entries successfully removed by the
\*              maintainer. Used in safety invariants.
\* phase      - workload phase, "running" or "drained". The producer fires
\*              while phase = "running" and the workload eventually
\*              transitions to "drained", after which only the maintainer
\*              can act. This makes the liveness obligation concrete: the
\*              maintainer must bring Excess back to 0 after the workload
\*              stops pushing - any failure to do so is a permanent stall.

VARIABLE oplogSize, attemptSize, truncated, phase

vars == <<oplogSize, attemptSize, truncated, phase>>

\* ---------------------------------------------------------------------------
\* Helpers
\* ---------------------------------------------------------------------------

\* Excess over retention cap. The cap maintainer's job is to eventually
\* drive this back to 0.
Excess == IF oplogSize > RetentionCap THEN oplogSize - RetentionCap ELSE 0

\* "Would this attempt fit in the cache budget?" Slow truncate fails when
\* the requested window exceeds the WiredTiger-bounded cache.
WouldFitCache(req) == req <= CacheBudget

\* Cap an attempt at the actual excess - asking for more than the excess
\* would mean truncating valid retention-window entries.
EffectiveAttempt(req) == IF req > Excess THEN Excess ELSE req

\* ---------------------------------------------------------------------------
\* Initial state
\* ---------------------------------------------------------------------------

Init ==
    /\ oplogSize    = 0
    /\ attemptSize  = RetentionCap
    /\ truncated    = 0
    /\ phase        = "running"

\* ---------------------------------------------------------------------------
\* Actions
\* ---------------------------------------------------------------------------

\* Producer appends 1..MaxBurst entries to the oplog. Disabled once the
\* workload has drained, and disabled when the model bound has been
\* reached so TLC's state space is finite.
ProducerAppend ==
    /\ phase = "running"
    /\ oplogSize < MaxOplogSize
    /\ \E n \in 1..MaxBurst :
         /\ oplogSize + n <= MaxOplogSize
         /\ oplogSize' = oplogSize + n
    /\ UNCHANGED <<attemptSize, truncated, phase>>

\* The workload eventually stops generating new oplog entries (e.g. the
\* application is taken down). After this point the maintainer is the
\* only actor and is solely responsible for satisfying liveness.
WorkloadDrains ==
    /\ phase = "running"
    /\ phase' = "drained"
    /\ UNCHANGED <<oplogSize, attemptSize, truncated>>

\* The maintainer wakes, sees excess > 0, and attempts to truncate.
\* If the attempt fits in cache it succeeds. If not, it raises a WCE.
\* The two cases are split into separate enabled actions so TLC can
\* enumerate them.

\* Success path: requested window fits the per-cycle cache budget.
\* The oplog shrinks by EffectiveAttempt(attemptSize). The maintainer
\* resets its next requested size to RetentionCap (so subsequent cycles
\* request only the current excess).
MaintainerTruncateSuccess ==
    /\ Excess > 0
    /\ WouldFitCache(attemptSize)
    /\ LET delta == EffectiveAttempt(attemptSize)
       IN  /\ delta > 0
           /\ oplogSize'   = oplogSize - delta
           /\ truncated'   = truncated + delta
           /\ attemptSize' = RetentionCap
    /\ UNCHANGED phase

\* WCE path: requested window exceeds the cache budget. No bytes are
\* truncated. The fix (SERVER-122519, "Change truncation thread interval
\* to 30s when doing time-based truncation in disagg") is modeled here
\* as: drop attemptSize to CacheBudget. That guarantees the next cycle
\* fits the cache and makes progress. In the bug-config the action is
\* disabled (BackoffOnConflict = FALSE) so the stuck state has no
\* outgoing edge - which is what TLC will report as a liveness
\* counter-example to EventuallyTruncates.
MaintainerTruncateConflict ==
    /\ BackoffOnConflict
    /\ Excess > 0
    /\ ~ WouldFitCache(attemptSize)
    /\ oplogSize'   = oplogSize
    /\ truncated'   = truncated
    /\ attemptSize' = CacheBudget
    /\ UNCHANGED phase

\* On the very first wake-up after retention is enabled (or after a
\* successful truncation), the maintainer asks for the full excess at
\* once. This is the literal behavior described in Nick Shectman's
\* 2026-03-10 comment: "the cap maintainer thread will attempt to
\* truncate away all of the excess oplog at once". The pre-condition
\* attemptSize <= RetentionCap ensures the action only fires from the
\* "reset" state (start-up or post-success) - it does NOT overwrite a
\* backed-off attemptSize, otherwise the spec would oscillate between
\* backoff and re-inflation and never converge.
MaintainerRecomputeWindow ==
    /\ Excess > 0
    /\ attemptSize <= RetentionCap
    /\ Excess > RetentionCap
    /\ attemptSize' = Excess
    /\ UNCHANGED <<oplogSize, truncated, phase>>

Next ==
    \/ ProducerAppend
    \/ WorkloadDrains
    \/ MaintainerTruncateSuccess
    \/ MaintainerTruncateConflict
    \/ MaintainerRecomputeWindow

\* Fairness:
\*   - The workload must eventually drain (otherwise the producer could
\*     refuse to stop and trivially keep Excess > 0 forever, but that's
\*     not the bug we're modeling).
\*   - The maintainer thread is required to run, by spec. Without WF on
\*     the maintainer actions we'd get a trivial "scheduler never runs
\*     the thread" counter-example, which also isn't the bug.
Fairness ==
    /\ WF_vars(WorkloadDrains)
    /\ WF_vars(MaintainerTruncateSuccess)
    /\ WF_vars(MaintainerTruncateConflict)
    /\ WF_vars(MaintainerRecomputeWindow)

Spec == Init /\ [][Next]_vars /\ Fairness

\* ---------------------------------------------------------------------------
\* Safety invariants
\* ---------------------------------------------------------------------------

\* Type / bounds invariant.
TypeOK ==
    /\ oplogSize    \in 0..MaxOplogSize
    /\ attemptSize  \in 1..MaxOplogSize
    /\ truncated    \in Nat
    /\ phase        \in {"running", "drained"}

\* The maintainer never removes entries we must retain. (We model "must
\* retain" as RetentionCap entries; in production those are the entries
\* younger than MinRetentionHours.)
NeverTruncateRetained ==
    oplogSize >= 0 /\ (oplogSize >= RetentionCap \/ truncated = 0 \/ RetentionCap = 0)

\* ---------------------------------------------------------------------------
\* Liveness property - the obligation that the prod incident violated
\* ---------------------------------------------------------------------------

\* If the oplog ever exceeds the retention cap, eventually the maintainer
\* shrinks it back. This is the patent claim of the cap-maintainer thread,
\* and it's the property six clusters in the fleet were not meeting in
\* March 2026.
EventuallyTruncates ==
    [](Excess > 0 ~> Excess = 0)

===============================================================================
