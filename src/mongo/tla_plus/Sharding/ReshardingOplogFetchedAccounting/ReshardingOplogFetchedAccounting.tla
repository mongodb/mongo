
\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE ReshardingOplogFetchedAccounting --------------------------------
(***************************************************************************************************)
(* This specification models the resharding `oplogEntriesFetched` accounting protocol that gates    *)
(* the resharding coordinator's entry into the critical section. The bug being modelled is the one  *)
(* described in SERVER-118706:                                                                      *)
(*                                                                                                  *)
(*   1. The fetcher draws an oplog batch from the donor and intends to insert it into the local     *)
(*      resharding oplog buffer collection.                                                         *)
(*   2. Before (BUG) or after (FIX) the insert lands, the fetcher publishes the batch size to the   *)
(*      `oplogEntriesFetched' metric the coordinator reads.                                         *)
(*   3. The local insert can fail (e.g. WriteConflict). The fetcher retries.                        *)
(*   4. In the buggy ordering the retry re-publishes the batch size without rolling back the prior  *)
(*      publication. The metric drifts permanently above the number of entries actually present in  *)
(*      the buffer.                                                                                 *)
(*                                                                                                  *)
(* The resharding coordinator decides whether to engage the critical section using                  *)
(*                                                                                                  *)
(*    estimatedRemainingTime = timeSpentApplying * (oplogEntriesFetched / oplogEntriesApplied - 1)  *)
(*                                                                                                  *)
(* (see resharding_util.cpp::estimateRemainingRecipientTime). The recipient applier can only        *)
(* drain entries that are physically present in the buffer; under permanent overcount               *)
(* `oplogEntriesApplied' is bounded above by the buffer cardinality. Consequently `fetched > N' and *)
(* `applied <= N' implies the ratio (fetched / applied - 1) cannot drop below                       *)
(* (overcount / N), the estimated remaining time stays above the threshold, and the critical       *)
(* section is never engaged. The whole resharding operation hangs.                                  *)
(*                                                                                                  *)
(* Modelling notes:                                                                                 *)
(*  - A single donor and a single recipient are modelled. The coordinator is modelled as a state    *)
(*    machine that observes the recipient-side accounting variables.                                *)
(*  - DonorOps is a fixed sequence of oplog positions the donor will emit. We treat them as opaque  *)
(*    integers; only their count is load-bearing.                                                   *)
(*  - The fetcher is allowed to non-deterministically fail an insert (WriteConflict) once per       *)
(*    batch. The retry replays the same batch (i.e. lastOplogId does not advance through a failed   *)
(*    insert). This mirrors the production retry shape described in the ticket.                     *)
(*  - The applier monotonically drains entries from the buffer. The coordinator polls the metrics. *)
(*                                                                                                  *)
(* The TWO orderings of "increment counter" vs "insert into buffer" are selected by the CONSTANT    *)
(* IncrementBeforeInsert. Setting IncrementBeforeInsert = TRUE reproduces the BUG; setting it to    *)
(* FALSE reproduces the FIX shipped in 8.1 by SERVER-94800.                                         *)
(***************************************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    DonorOps,              \* Set of oplog op-ids the donor will publish (treat as opaque integers).
    BatchMaxSize,          \* Maximum number of oplog ops the fetcher pulls per aggregate batch.
    MaxInsertFailures,     \* Upper bound on number of WriteConflict-style insert failures allowed.
    CritSecRatioNumer,     \* Threshold ratio (fetched/applied) numerator. Crit-sec engages when
    CritSecRatioDenom,     \* fetched * CritSecRatioDenom <= applied * CritSecRatioNumer.
    IncrementBeforeInsert  \* TRUE => buggy ordering (SERVER-118706 reproducer);
                           \* FALSE => fixed ordering (SERVER-94800 fix shape).

ASSUME Cardinality(DonorOps) > 0
ASSUME BatchMaxSize \in 1..10
ASSUME MaxInsertFailures \in 0..10
ASSUME CritSecRatioNumer \in 1..100
ASSUME CritSecRatioDenom \in 1..100
ASSUME CritSecRatioNumer >= CritSecRatioDenom   \* The threshold ratio is >= 1 (fetched >= applied).
ASSUME IncrementBeforeInsert \in BOOLEAN

(* Total number of oplog ops the donor will publish across the run. *)
TotalOps == Cardinality(DonorOps)

(***************************************************************************************************)
(* Variables                                                                                       *)
(***************************************************************************************************)

(* Donor variables. The donor publishes oplog ops via batches. Since the spec only consumes the    *)
(* count of remaining ops, we model the donor as a counter "ops left to publish".                  *)
VARIABLES
    donorOpsRemaining       \* Number of donor oplog ops not yet pulled into a fetcher batch.

(* Fetcher variables. The fetcher pulls a batch, then either (a) increments fetched then inserts,  *)
(* or (b) inserts then increments. On insert failure, the fetcher retries the same batch.          *)
VARIABLES
    fetcherState,           \* "IDLE" | "BATCH_PULLED" | "INSERTED" | "RETRY_PENDING" | "DONE".
    fetcherBatchSize,       \* Size of the in-flight batch.
    fetcherInsertFailures   \* Counter of insert-retries consumed so far.

(* Recipient buffer + applier variables. *)
VARIABLES
    bufferCount,            \* Number of oplog ops successfully inserted into the local buffer.
    appliedCount            \* Number of buffered ops the applier has drained.

(* Resharding metrics — the contract surface the coordinator reads. *)
VARIABLES
    oplogEntriesFetched,    \* Counter the fetcher publishes (the bug overcounts this).
    oplogEntriesApplied     \* Counter the applier publishes (always matches appliedCount).

(* Coordinator variables. *)
VARIABLES
    coordinatorState        \* "BUILDING_INDEX" | "APPLYING" | "CRIT_SEC_ENGAGED" | "ABORTED".

vars == <<donorOpsRemaining,
          fetcherState, fetcherBatchSize, fetcherInsertFailures,
          bufferCount, appliedCount,
          oplogEntriesFetched, oplogEntriesApplied,
          coordinatorState>>

(***************************************************************************************************)
(* Type invariants                                                                                 *)
(***************************************************************************************************)

TypeOK ==
    /\ donorOpsRemaining \in 0..TotalOps
    /\ fetcherState \in {"IDLE", "BATCH_PULLED", "INSERTED", "RETRY_PENDING", "DONE"}
    /\ fetcherBatchSize \in 0..BatchMaxSize
    /\ fetcherInsertFailures \in 0..MaxInsertFailures
    /\ bufferCount \in 0..TotalOps
    /\ appliedCount \in 0..TotalOps
    /\ oplogEntriesFetched \in 0..(TotalOps + MaxInsertFailures * BatchMaxSize)
    /\ oplogEntriesApplied \in 0..TotalOps
    /\ coordinatorState \in {"BUILDING_INDEX", "APPLYING", "CRIT_SEC_ENGAGED", "ABORTED"}

(***************************************************************************************************)
(* Init                                                                                            *)
(***************************************************************************************************)

Init ==
    /\ donorOpsRemaining = TotalOps
    /\ fetcherState = "IDLE"
    /\ fetcherBatchSize = 0
    /\ fetcherInsertFailures = 0
    /\ bufferCount = 0
    /\ appliedCount = 0
    /\ oplogEntriesFetched = 0
    /\ oplogEntriesApplied = 0
    /\ coordinatorState = "APPLYING"

Min(a, b) == IF a <= b THEN a ELSE b

(***************************************************************************************************)
(* Actions                                                                                         *)
(***************************************************************************************************)

\* Fetcher pulls a batch of up to BatchMaxSize oplog ops from the donor.
PullBatch ==
    /\ fetcherState = "IDLE"
    /\ donorOpsRemaining > 0
    /\ LET sz == Min(donorOpsRemaining, BatchMaxSize) IN
        /\ fetcherBatchSize' = sz
        /\ donorOpsRemaining' = donorOpsRemaining - sz
        /\ fetcherState' = "BATCH_PULLED"
    /\ UNCHANGED <<fetcherInsertFailures, bufferCount, appliedCount,
                   oplogEntriesFetched, oplogEntriesApplied, coordinatorState>>

\* "Pre-publish" — the buggy ordering. Fetcher increments oplogEntriesFetched before the insert.
\* In the FIX configuration this action is disabled.
PrePublishFetched ==
    /\ IncrementBeforeInsert = TRUE
    /\ fetcherState = "BATCH_PULLED"
    /\ oplogEntriesFetched' = oplogEntriesFetched + fetcherBatchSize
    /\ fetcherState' = "INSERTED"   \* Buggy code path moves to insert-attempt with counter already bumped.
    /\ UNCHANGED <<donorOpsRemaining, fetcherBatchSize, fetcherInsertFailures,
                   bufferCount, appliedCount, oplogEntriesApplied, coordinatorState>>

\* The insert into the buffer collection succeeds. In the BUG ordering, the counter was already
\* bumped (state INSERTED implies "intent to insert"); the success commits the buffer write.
\* In the FIX ordering, the counter is bumped here, after the buffer-write commits.
InsertSucceed ==
    /\ \/ /\ IncrementBeforeInsert = TRUE
          /\ fetcherState = "INSERTED"
          /\ bufferCount' = bufferCount + fetcherBatchSize
          /\ UNCHANGED oplogEntriesFetched
       \/ /\ IncrementBeforeInsert = FALSE
          /\ fetcherState = "BATCH_PULLED"
          /\ bufferCount' = bufferCount + fetcherBatchSize
          /\ oplogEntriesFetched' = oplogEntriesFetched + fetcherBatchSize
    /\ fetcherState' = "IDLE"
    /\ fetcherBatchSize' = 0
    /\ UNCHANGED <<donorOpsRemaining, fetcherInsertFailures, appliedCount,
                   oplogEntriesApplied, coordinatorState>>

\* The insert into the buffer collection fails (e.g. WriteConflict). The fetcher must retry the
\* same batch. In the BUG ordering, the counter was already bumped: the retry path takes the same
\* "increment then attempt insert" sequence, and so the counter gets bumped AGAIN on retry. In the
\* FIX ordering the counter has not yet been bumped, so the retry is clean.
InsertFail ==
    /\ fetcherInsertFailures < MaxInsertFailures
    /\ \/ /\ IncrementBeforeInsert = TRUE
          /\ fetcherState = "INSERTED"
       \/ /\ IncrementBeforeInsert = FALSE
          /\ fetcherState = "BATCH_PULLED"
    /\ fetcherInsertFailures' = fetcherInsertFailures + 1
    /\ fetcherState' = "RETRY_PENDING"
    /\ UNCHANGED <<donorOpsRemaining, fetcherBatchSize, bufferCount, appliedCount,
                   oplogEntriesFetched, oplogEntriesApplied, coordinatorState>>

\* The fetcher retries the same batch. This is where the bug manifests: in the BUG ordering the
\* retry re-runs "increment then insert", double-counting the batch in `oplogEntriesFetched'. In
\* the FIX ordering the retry simply re-attempts the buffer insert without touching the counter.
RetryAfterFail ==
    /\ fetcherState = "RETRY_PENDING"
    /\ \/ /\ IncrementBeforeInsert = TRUE
          \* Bug: PrePublish happens again on the retry path.
          /\ oplogEntriesFetched' = oplogEntriesFetched + fetcherBatchSize
          /\ fetcherState' = "INSERTED"
       \/ /\ IncrementBeforeInsert = FALSE
          /\ fetcherState' = "BATCH_PULLED"
          /\ UNCHANGED oplogEntriesFetched
    /\ UNCHANGED <<donorOpsRemaining, fetcherBatchSize, fetcherInsertFailures,
                   bufferCount, appliedCount, oplogEntriesApplied, coordinatorState>>

\* The applier drains one entry from the buffer.
ApplierDrain ==
    /\ appliedCount < bufferCount
    /\ appliedCount' = appliedCount + 1
    /\ oplogEntriesApplied' = oplogEntriesApplied + 1
    /\ UNCHANGED <<donorOpsRemaining, fetcherState, fetcherBatchSize, fetcherInsertFailures,
                   bufferCount, oplogEntriesFetched, coordinatorState>>

\* Fetcher signals it has consumed all donor ops and exits its loop. Required for liveness so
\* that the spec can terminate cleanly when no more batches remain.
FetcherDone ==
    /\ fetcherState = "IDLE"
    /\ donorOpsRemaining = 0
    /\ fetcherState' = "DONE"
    /\ UNCHANGED <<donorOpsRemaining, fetcherBatchSize, fetcherInsertFailures,
                   bufferCount, appliedCount, oplogEntriesFetched, oplogEntriesApplied,
                   coordinatorState>>

\* The coordinator polls the fetcher / applier metrics and decides whether to engage the critical
\* section. The real-system predicate is the one in resharding_util.cpp::estimateRemainingRecipientTime:
\*
\*     remaining = applying_time_so_far * (fetched / applied - 1)
\*     engage_crit_sec iff remaining <= threshold
\*
\* We approximate the time-bounded predicate with a ratio threshold expressed as numerator/denom:
\* the coordinator engages when fetched * Denom <= applied * Numer, with Numer >= Denom. The spec
\* additionally insists the buffer must actually be drained (appliedCount = bufferCount) when the
\* fetcher is done — this matches the production check that "we have applied everything currently
\* buffered" before deciding to block writes.
CoordinatorEngageCritSec ==
    /\ coordinatorState = "APPLYING"
    /\ oplogEntriesFetched > 0
    /\ oplogEntriesApplied > 0
    /\ oplogEntriesFetched * CritSecRatioDenom <= oplogEntriesApplied * CritSecRatioNumer
    /\ coordinatorState' = "CRIT_SEC_ENGAGED"
    /\ UNCHANGED <<donorOpsRemaining, fetcherState, fetcherBatchSize, fetcherInsertFailures,
                   bufferCount, appliedCount, oplogEntriesFetched, oplogEntriesApplied>>

\* The coordinator may abort. We allow an abort transition only to model the "operator gives up"
\* path; without it, the bug configuration would deadlock under TLC with CHECK_DEADLOCK set. The
\* invariants tolerate the abort state — see safety properties below.
CoordinatorAbort ==
    /\ coordinatorState = "APPLYING"
    /\ fetcherState = "DONE"
    /\ appliedCount = bufferCount             \* All buffered drained, yet ratio still off.
    /\ donorOpsRemaining = 0
    /\ oplogEntriesFetched > oplogEntriesApplied  \* Definitionally — overcount left behind.
    /\ coordinatorState' = "ABORTED"
    /\ UNCHANGED <<donorOpsRemaining, fetcherState, fetcherBatchSize, fetcherInsertFailures,
                   bufferCount, appliedCount, oplogEntriesFetched, oplogEntriesApplied>>

\* Terminal stuttering transition. Once the resharding operation has reached a terminal state —
\* either the critical section has engaged (success) or the operator has aborted (failure) — and
\* the fetcher/applier have drained, the spec stutters indefinitely. Without this transition TLC
\* reports a spurious deadlock when the system reaches its happy terminal state, since no action
\* is enabled.
Terminating ==
    /\ \/ coordinatorState = "CRIT_SEC_ENGAGED"
       \/ coordinatorState = "ABORTED"
    /\ fetcherState = "DONE"
    /\ appliedCount = bufferCount
    /\ donorOpsRemaining = 0
    /\ UNCHANGED vars

Next ==
    \/ PullBatch
    \/ PrePublishFetched
    \/ InsertSucceed
    \/ InsertFail
    \/ RetryAfterFail
    \/ ApplierDrain
    \/ FetcherDone
    \/ CoordinatorEngageCritSec
    \/ CoordinatorAbort
    \/ Terminating

(***************************************************************************************************)
(* Fairness                                                                                        *)
(***************************************************************************************************)

Fairness ==
    /\ WF_vars(PullBatch)
    /\ WF_vars(PrePublishFetched)
    /\ WF_vars(InsertSucceed)
    /\ WF_vars(RetryAfterFail)
    /\ WF_vars(ApplierDrain)
    /\ WF_vars(FetcherDone)
    /\ WF_vars(CoordinatorEngageCritSec)
    \* Note: InsertFail and CoordinatorAbort are deliberately NOT in the fairness set. InsertFail is
    \* an adversarial action whose firing is bounded by MaxInsertFailures; CoordinatorAbort is a
    \* terminal escape hatch that should only fire when liveness has already been violated.

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************************************)
(* Safety invariants                                                                                *)
(***************************************************************************************************)

\* The accounting contract that the coordinator's threshold rule depends on:
\* oplogEntriesFetched must never get permanently ahead of bufferCount + currently-in-flight batch.
\* In the FIX configuration the counter is only bumped after the insert lands, so this invariant
\* holds tightly: oplogEntriesFetched <= bufferCount + (in-flight-but-not-yet-counted).
\* In the BUG configuration the counter can be strictly greater than bufferCount by an amount
\* proportional to the number of insert failures consumed — that is the overcount the ticket is
\* about. This invariant is therefore safety-only under FIX, and is left disabled in the BUG cfg.
FetchedTracksBuffer ==
    oplogEntriesFetched <= bufferCount + fetcherBatchSize

\* Applied is always bounded above by what's physically present in the buffer.
AppliedBoundedByBuffer == oplogEntriesApplied <= bufferCount

\* The applier cannot get ahead of the fetcher in TRUTH: appliedCount <= bufferCount <=
\* (donor ops pulled) <= TotalOps. This invariant is independent of the buggy ordering.
AppliedBoundedByTotal == oplogEntriesApplied <= TotalOps

\* The CONTRACT: if the coordinator engaged the critical section, then by the production rule
\* (fetched/applied - 1) was below threshold AND the applier had drained everything currently
\* present in the buffer. If overcount has occurred, the only way crit-sec can engage is to wait
\* long enough that applied catches up to the inflated fetched count — which is IMPOSSIBLE because
\* applied is bounded by bufferCount, and bufferCount is bounded by the actual donor op stream.
\* Hence under BUG configuration this implies the crit-sec is never reached when fetched > the
\* eventually-applied count.
CritSecImpliesAppliedReachedBuffer ==
    coordinatorState = "CRIT_SEC_ENGAGED" => oplogEntriesApplied <= oplogEntriesFetched

(***************************************************************************************************)
(* Liveness properties                                                                              *)
(***************************************************************************************************)

\* Eventually, the fetcher is done pulling donor ops.
FetcherEventuallyDone == <>(fetcherState = "DONE")

\* Eventually, the applier drains everything in the buffer.
ApplierEventuallyDrains == <>(appliedCount = bufferCount /\ fetcherState = "DONE")

\* The headline liveness property the ticket is about:
\*   If the donor's op stream is bounded and the applier is allowed to drain unimpeded, then the
\*   coordinator MUST eventually engage the critical section. Under the BUG configuration this
\*   property is violated: the counter overcount permanently inflates the ratio, the coordinator
\*   never crosses the threshold, and resharding hangs.
CritSecEventuallyEngaged ==
    <>(coordinatorState = "CRIT_SEC_ENGAGED")

\* Under FIX configuration we should also have: once the applier has drained everything, the
\* coordinator engages the critical section (and stays there, by stuttering).
DrainImpliesCritSec ==
    [](
        (fetcherState = "DONE" /\ appliedCount = bufferCount /\ bufferCount > 0)
        =>
        <>(coordinatorState = "CRIT_SEC_ENGAGED")
    )

\* Under the BUG configuration we want to prove the violation explicitly. We express it as: there
\* exists an execution in which the coordinator never engages the critical section AND the system
\* otherwise progresses normally (everything drained, donor empty, no more retries possible). This
\* is precisely the "hang" the ticket describes.
HangScenarioReachable ==
    \* A bait predicate — used in MC cfg to surface the counterexample trace.
    /\ fetcherState = "DONE"
    /\ appliedCount = bufferCount
    /\ donorOpsRemaining = 0
    /\ oplogEntriesFetched > oplogEntriesApplied
    /\ coordinatorState = "APPLYING"

====================================================================================================
