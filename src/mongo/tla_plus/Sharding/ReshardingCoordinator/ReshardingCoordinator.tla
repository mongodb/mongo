\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE ReshardingCoordinator ----------------------------------------
\* This specification models the resharding coordinator state machine and its interaction with
\* donors and recipients across stepdown / stepup / abort.
\*
\* References:
\* - src/mongo/db/s/resharding/resharding_coordinator.h (CoordinatorCancellationTokenHolder)
\* - src/mongo/db/s/resharding/resharding_coordinator_service.{h,cpp}
\* - src/mongo/db/s/resharding/resharding_donor_service.cpp
\* - src/mongo/s/resharding/common_types.idl (CoordinatorState / DonorState / RecipientState enums)
\*
\* Scope:
\* - One in-flight resharding operation, one Coordinator, two Donors, two Recipients.
\* - State per actor: a phase position (the "state" field in the coordinator/donor/recipient
\*   documents), a `hasCancelSource' boolean (whether the actor's CancellationSource has been
\*   initialized post-stepup; see CoordinatorCancellationTokenHolder ctor in
\*   resharding_coordinator.h), and a `pendingAbort' boolean (whether a
\*   _shardsvrAbortReshardCollection request has arrived but has not yet been dispatched to a
\*   cancellation source).
\* - Actions: Stepdown, Stepup, InitCancelSource, AbortRequest, AdvanceState, Commit, Done.
\*
\* SERVER-115139 race:
\*   "ReshardingDonorService cannot reliably handle the case where _shardsvrAbortReshardCollection
\*   command comes in before the cancellation source is initialized upon stepup recovery."
\*
\* Encoded here as: after Stepup(d) the donor `d' has its state restored from disk but
\* `hasCancelSource[d] = FALSE' until `InitCancelSource(d)' fires. The buggy execution allows
\* `AbortRequest(d)' to land in that gap; the bug-mode cfg checks
\* `AbortAlwaysHandled' and produces a counterexample where an abort is dropped.
\*
\* To run:
\*     cd src/mongo/tla_plus
\*     ./download-tlc.sh
\*     ./model-check.sh Sharding/ReshardingCoordinator

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Coordinator,        \* The single resharding coordinator actor (singleton set).
    Donors,             \* The set of donor shards for this operation.
    Recipients,         \* The set of recipient shards for this operation.
    MaxStepdowns,       \* Bound on Stepdown events, for state-space finiteness.
    BugMode             \* TRUE: model the SERVER-115139 race (abort can drop in init gap).
                        \* FALSE: model the fixed implementation (abort queues until init).

ASSUME Cardinality(Coordinator) = 1
ASSUME Cardinality(Donors) >= 1
ASSUME Cardinality(Recipients) >= 1
ASSUME MaxStepdowns \in 0..5
ASSUME BugMode \in BOOLEAN

----------------------------------------------------------------------------------------------------
(* Phase vocabularies                                                                             *)

\* Coordinator phases (subset of CoordinatorStateEnum used by this spec).
CoordUnused           == "coordUnused"
CoordInitializing     == "coordInitializing"
CoordPreparingToDonate== "coordPreparingToDonate"
CoordCloning          == "coordCloning"
CoordApplying         == "coordApplying"
CoordBlockingWrites   == "coordBlockingWrites"
CoordCommitting       == "coordCommitting"
CoordAborting         == "coordAborting"
CoordDone             == "coordDone"

CoordPhases == { CoordUnused, CoordInitializing, CoordPreparingToDonate, CoordCloning,
                 CoordApplying, CoordBlockingWrites, CoordCommitting, CoordAborting, CoordDone }

\* Coordinator phase ordering for "advance" actions. CoordAborting and CoordDone are terminal.
CoordOrder == << CoordUnused, CoordInitializing, CoordPreparingToDonate, CoordCloning,
                 CoordApplying, CoordBlockingWrites, CoordCommitting, CoordDone >>

CoordPhaseIdx(p) == CHOOSE i \in 1..Len(CoordOrder) : CoordOrder[i] = p

\* Donor phases (DonorStateEnum, subset).
DonorUnused           == "donorUnused"
DonorPreparingToDonate== "donorPreparingToDonate"
DonorDonatingInitial  == "donorDonatingInitialData"
DonorDonatingOplog    == "donorDonatingOplogEntries"
DonorBlockingWrites   == "donorBlockingWrites"
DonorError            == "donorError"
DonorDone             == "donorDone"

DonorPhases == { DonorUnused, DonorPreparingToDonate, DonorDonatingInitial, DonorDonatingOplog,
                 DonorBlockingWrites, DonorError, DonorDone }

DonorOrder == << DonorUnused, DonorPreparingToDonate, DonorDonatingInitial, DonorDonatingOplog,
                 DonorBlockingWrites, DonorDone >>

DonorPhaseIdx(p) == CHOOSE i \in 1..Len(DonorOrder) : DonorOrder[i] = p

\* Recipient phases (RecipientStateEnum, subset).
RecipUnused           == "recipUnused"
RecipAwaitingFetch    == "recipAwaitingFetchTimestamp"
RecipCloning          == "recipCloning"
RecipApplying         == "recipApplying"
RecipStrictConsistency== "recipStrictConsistency"
RecipError            == "recipError"
RecipDone             == "recipDone"

RecipPhases == { RecipUnused, RecipAwaitingFetch, RecipCloning, RecipApplying,
                 RecipStrictConsistency, RecipError, RecipDone }

RecipOrder == << RecipUnused, RecipAwaitingFetch, RecipCloning, RecipApplying,
                 RecipStrictConsistency, RecipDone >>

RecipPhaseIdx(p) == CHOOSE i \in 1..Len(RecipOrder) : RecipOrder[i] = p

----------------------------------------------------------------------------------------------------
(* Variables                                                                                      *)

\* Per-actor durable state position (the document on disk: config.reshardingOperations for the
\* coordinator, config.localReshardingOperations.{donor,recipient} for the participants).
VARIABLE coordState
VARIABLE donorState
VARIABLE recipState

\* Per-actor primary status. When primary[a] = FALSE, the actor is in a stepped-down state and is
\* not driving transitions. When primary[a] flips TRUE, the actor must InitCancelSource before it
\* can dispatch any further token-bearing work.
VARIABLE primary

\* Per-actor hasCancelSource[a]: whether the actor's CancellationSource (the _abortSource in
\* CoordinatorCancellationTokenHolder) has been constructed for the current primary term. This
\* flips FALSE on stepdown and FALSE on stepup; only InitCancelSource(a) flips it TRUE.
VARIABLE hasCancelSource

\* Per-actor pendingAbort[a]: an inbound _shardsvrAbortReshardCollection that has been received but
\* whose dispatch to the abort token is pending. In the bug model, an abort that arrives while
\* `~hasCancelSource[a] /\ ~primary[a]' (stepdown gap) OR `~hasCancelSource[a] /\ primary[a]' (init
\* gap) is silently dropped. In the fixed model the abort queues in pendingAbort and is consumed by
\* InitCancelSource.
VARIABLE pendingAbort

\* Global: whether an abort has been requested at least once for this operation. This is the
\* operator/router intent that must not be lost. Once requested it stays requested forever.
VARIABLE abortRequested

\* Global: whether an abort that was requested has been observed/handled by *some* actor (i.e.
\* dispatched onto a cancellation source somewhere in the system). The AbortAlwaysHandled invariant
\* says: abortRequested => eventually abortObserved. We use a safety surrogate: at every point
\* where the coordinator advances past CoordPreparingToDonate, abortObserved must hold or
\* abortRequested must still be FALSE. See AbortAlwaysHandled below.
VARIABLE abortObserved

\* Bound counter: how many Stepdown events have occurred (across all actors). Caps state space.
VARIABLE stepdownCount

\* Whether the coordinator has reached its terminal CoordDone (success) state. Used in invariants.
VARIABLE committed

vars == << coordState, donorState, recipState, primary, hasCancelSource, pendingAbort,
           abortRequested, abortObserved, stepdownCount, committed >>

Actors == Coordinator \cup Donors \cup Recipients

----------------------------------------------------------------------------------------------------
(* Helpers                                                                                        *)

TheCoordinator == CHOOSE c \in Coordinator : TRUE

\* Stepup gap predicate: actor is primary but cancellation source not initialized.
InStepupGap(a) == primary[a] /\ ~hasCancelSource[a]

\* Stepdown gap predicate: actor is not primary (so any inbound abort cannot be dispatched until
\* a future Stepup + InitCancelSource).
InStepdownGap(a) == ~primary[a]

\* Predicate: the coordinator is in an active (non-terminal) phase.
CoordActive ==
    /\ coordState # CoordDone
    /\ coordState # CoordAborting

\* Predicate: the coordinator is past the early-cloning point. Used by the safety surrogate.
CoordPastInit == CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordCloning)

\* All donors at or past a given phase index.
AllDonorsAtLeast(idx) == \A d \in Donors : DonorPhaseIdx(donorState[d]) >= idx

\* All recipients at or past a given phase index.
AllRecipientsAtLeast(idx) == \A r \in Recipients : RecipPhaseIdx(recipState[r]) >= idx

----------------------------------------------------------------------------------------------------
(* Init                                                                                           *)

Init ==
    /\ coordState = CoordUnused
    /\ donorState = [d \in Donors |-> DonorUnused]
    /\ recipState = [r \in Recipients |-> RecipUnused]
    /\ primary = [a \in Actors |-> TRUE]
    /\ hasCancelSource = [a \in Actors |-> TRUE]
    /\ pendingAbort = [a \in Actors |-> FALSE]
    /\ abortRequested = FALSE
    /\ abortObserved = FALSE
    /\ stepdownCount = 0
    /\ committed = FALSE

----------------------------------------------------------------------------------------------------
(* Actions                                                                                        *)

\* Coordinator advances from its current phase to the next phase in CoordOrder.
\* Gated on: primary, hasCancelSource, all relevant participants at-or-past the matching phase.
\* No advance is permitted while abortRequested holds (the coordinator must enter aborting first).
CoordAdvance ==
    /\ primary[TheCoordinator]
    /\ hasCancelSource[TheCoordinator]
    /\ CoordActive
    /\ ~abortRequested
    /\ LET i == CoordPhaseIdx(coordState)
           next == CoordOrder[i + 1]
       IN  /\ i + 1 <= Len(CoordOrder)
           /\ next # CoordDone   \* CoordDone is reached only via the explicit Commit action.
           /\ \/ next \in { CoordInitializing, CoordPreparingToDonate, CoordCloning }
              \/ /\ next = CoordApplying
                 /\ AllRecipientsAtLeast(RecipPhaseIdx(RecipCloning))
              \/ /\ next = CoordBlockingWrites
                 /\ AllRecipientsAtLeast(RecipPhaseIdx(RecipApplying))
              \/ /\ next = CoordCommitting
                 /\ AllDonorsAtLeast(DonorPhaseIdx(DonorBlockingWrites))
                 /\ AllRecipientsAtLeast(RecipPhaseIdx(RecipStrictConsistency))
           /\ coordState' = next
    /\ UNCHANGED << donorState, recipState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

\* Donor advances along DonorOrder, gated on primary + hasCancelSource and the coordinator having
\* reached an appropriately-permissive phase.
DonorAdvance(d) ==
    /\ d \in Donors
    /\ primary[d]
    /\ hasCancelSource[d]
    /\ donorState[d] # DonorDone
    /\ donorState[d] # DonorError
    /\ ~abortRequested
    /\ LET i == DonorPhaseIdx(donorState[d])
           next == DonorOrder[i + 1]
       IN  /\ i + 1 <= Len(DonorOrder)
           /\ next # DonorDone
           /\ \/ next \in { DonorPreparingToDonate, DonorDonatingInitial }
              \/ /\ next = DonorDonatingOplog
                 /\ CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordApplying)
              \/ /\ next = DonorBlockingWrites
                 /\ CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordBlockingWrites)
           /\ donorState' = [donorState EXCEPT ![d] = next]
    /\ UNCHANGED << coordState, recipState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

\* Recipient advances along RecipOrder, gated on primary + hasCancelSource and the coordinator
\* having reached an appropriately-permissive phase.
RecipAdvance(r) ==
    /\ r \in Recipients
    /\ primary[r]
    /\ hasCancelSource[r]
    /\ recipState[r] # RecipDone
    /\ recipState[r] # RecipError
    /\ ~abortRequested
    /\ LET i == RecipPhaseIdx(recipState[r])
           next == RecipOrder[i + 1]
       IN  /\ i + 1 <= Len(RecipOrder)
           /\ next # RecipDone
           /\ \/ next \in { RecipAwaitingFetch, RecipCloning }
              \/ /\ next = RecipApplying
                 /\ CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordApplying)
              \/ /\ next = RecipStrictConsistency
                 /\ CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordBlockingWrites)
           /\ recipState' = [recipState EXCEPT ![r] = next]
    /\ UNCHANGED << coordState, donorState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

\* An actor steps down. Its durable on-disk state is preserved (coordState / donorState /
\* recipState unchanged) but its in-memory CancellationSource is destroyed.
Stepdown(a) ==
    /\ a \in Actors
    /\ primary[a]
    /\ stepdownCount < MaxStepdowns
    /\ ~committed
    /\ primary' = [primary EXCEPT ![a] = FALSE]
    /\ hasCancelSource' = [hasCancelSource EXCEPT ![a] = FALSE]
    /\ stepdownCount' = stepdownCount + 1
    /\ UNCHANGED << coordState, donorState, recipState, pendingAbort,
                    abortRequested, abortObserved, committed >>

\* An actor steps up. Its on-disk state remains; hasCancelSource stays FALSE until
\* InitCancelSource. This is the window SERVER-115139 names.
Stepup(a) ==
    /\ a \in Actors
    /\ ~primary[a]
    /\ primary' = [primary EXCEPT ![a] = TRUE]
    /\ UNCHANGED << coordState, donorState, recipState, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

\* The actor constructs its CancellationSource on the stepup recovery path. If an abort was
\* queued in pendingAbort, it is observed at this moment (the fixed implementation).
InitCancelSource(a) ==
    /\ a \in Actors
    /\ primary[a]
    /\ ~hasCancelSource[a]
    /\ hasCancelSource' = [hasCancelSource EXCEPT ![a] = TRUE]
    /\ IF pendingAbort[a]
        THEN /\ abortObserved' = TRUE
             /\ pendingAbort' = [pendingAbort EXCEPT ![a] = FALSE]
        ELSE /\ UNCHANGED abortObserved
             /\ UNCHANGED pendingAbort
    /\ UNCHANGED << coordState, donorState, recipState, primary,
                    abortRequested, stepdownCount, committed >>

\* A _shardsvrAbortReshardCollection arrives at actor `a'. Three cases:
\* (1) hasCancelSource[a] /\ primary[a]: dispatched to the abort token; observed immediately.
\* (2) ~hasCancelSource[a] (init gap or stepdown gap):
\*       - Fixed model (BugMode=FALSE): queue in pendingAbort[a].
\*       - Bug model   (BugMode=TRUE):  drop the abort (the SERVER-115139 behavior). abortRequested
\*         still flips TRUE because the user/router believes the request was accepted (this is
\*         exactly the surprise the bug produces).
AbortRequest(a) ==
    /\ a \in Actors
    /\ ~abortRequested
    /\ abortRequested' = TRUE
    /\ IF hasCancelSource[a] /\ primary[a]
        THEN /\ abortObserved' = TRUE
             /\ UNCHANGED pendingAbort
        ELSE IF BugMode
            THEN /\ UNCHANGED abortObserved
                 /\ UNCHANGED pendingAbort
            ELSE /\ UNCHANGED abortObserved
                 /\ pendingAbort' = [pendingAbort EXCEPT ![a] = TRUE]
    /\ UNCHANGED << coordState, donorState, recipState, primary, hasCancelSource,
                    stepdownCount, committed >>

\* Once an abort has been observed, the coordinator transitions to CoordAborting and the
\* participants to their Error / Done terminals. Models the cleanup path.
CoordEnterAborting ==
    /\ abortObserved
    /\ CoordActive
    /\ primary[TheCoordinator]
    /\ hasCancelSource[TheCoordinator]
    /\ coordState' = CoordAborting
    /\ UNCHANGED << donorState, recipState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

\* Donor transitions to error after the coordinator entered aborting.
DonorEnterError(d) ==
    /\ d \in Donors
    /\ primary[d]
    /\ hasCancelSource[d]
    /\ coordState = CoordAborting
    /\ donorState[d] # DonorDone
    /\ donorState[d] # DonorError
    /\ donorState' = [donorState EXCEPT ![d] = DonorError]
    /\ UNCHANGED << coordState, recipState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

\* Recipient transitions to error after the coordinator entered aborting.
RecipEnterError(r) ==
    /\ r \in Recipients
    /\ primary[r]
    /\ hasCancelSource[r]
    /\ coordState = CoordAborting
    /\ recipState[r] # RecipDone
    /\ recipState[r] # RecipError
    /\ recipState' = [recipState EXCEPT ![r] = RecipError]
    /\ UNCHANGED << coordState, donorState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

\* Coordinator successfully commits the operation. Requires all participants ready and no abort
\* requested.
Commit ==
    /\ primary[TheCoordinator]
    /\ hasCancelSource[TheCoordinator]
    /\ coordState = CoordCommitting
    /\ ~abortRequested
    /\ AllDonorsAtLeast(DonorPhaseIdx(DonorBlockingWrites))
    /\ AllRecipientsAtLeast(RecipPhaseIdx(RecipStrictConsistency))
    /\ coordState' = CoordDone
    /\ committed' = TRUE
    /\ UNCHANGED << donorState, recipState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount >>

\* Donor terminates after the coordinator is Done or Aborting.
DonorDoneTransition(d) ==
    /\ d \in Donors
    /\ primary[d]
    /\ hasCancelSource[d]
    /\ donorState[d] # DonorDone
    /\ \/ coordState = CoordDone
       \/ /\ coordState = CoordAborting
          /\ donorState[d] = DonorError
    /\ donorState' = [donorState EXCEPT ![d] = DonorDone]
    /\ UNCHANGED << coordState, recipState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

\* Recipient terminates after the coordinator is Done or Aborting.
RecipDoneTransition(r) ==
    /\ r \in Recipients
    /\ primary[r]
    /\ hasCancelSource[r]
    /\ recipState[r] # RecipDone
    /\ \/ coordState = CoordDone
       \/ /\ coordState = CoordAborting
          /\ recipState[r] = RecipError
    /\ recipState' = [recipState EXCEPT ![r] = RecipDone]
    /\ UNCHANGED << coordState, donorState, primary, hasCancelSource, pendingAbort,
                    abortRequested, abortObserved, stepdownCount, committed >>

----------------------------------------------------------------------------------------------------
(* Next                                                                                           *)

Next ==
    \/ CoordAdvance
    \/ \E d \in Donors     : DonorAdvance(d)
    \/ \E r \in Recipients : RecipAdvance(r)
    \/ \E a \in Actors     : Stepdown(a)
    \/ \E a \in Actors     : Stepup(a)
    \/ \E a \in Actors     : InitCancelSource(a)
    \/ \E a \in Actors     : AbortRequest(a)
    \/ CoordEnterAborting
    \/ \E d \in Donors     : DonorEnterError(d)
    \/ \E r \in Recipients : RecipEnterError(r)
    \/ Commit
    \/ \E d \in Donors     : DonorDoneTransition(d)
    \/ \E r \in Recipients : RecipDoneTransition(r)

Fairness ==
    /\ WF_vars(\E a \in Actors : InitCancelSource(a))
    /\ WF_vars(\E a \in Actors : Stepup(a))
    /\ WF_vars(CoordEnterAborting)

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(* Type invariants                                                                                *)

TypeOK ==
    /\ coordState \in CoordPhases
    /\ donorState \in [Donors -> DonorPhases]
    /\ recipState \in [Recipients -> RecipPhases]
    /\ primary \in [Actors -> BOOLEAN]
    /\ hasCancelSource \in [Actors -> BOOLEAN]
    /\ pendingAbort \in [Actors -> BOOLEAN]
    /\ abortRequested \in BOOLEAN
    /\ abortObserved \in BOOLEAN
    /\ stepdownCount \in 0..MaxStepdowns
    /\ committed \in BOOLEAN

----------------------------------------------------------------------------------------------------
(* Correctness Properties (invariants)                                                            *)

\* Safety surrogate for "no abort request is lost".
\*
\* If an abort has been requested and any actor is past the init/stepdown gap and has reached a
\* steady-state phase, then the abort must already be observed or pending somewhere.
\*
\* Stated as: it is not the case that (abortRequested holds AND every actor's hasCancelSource is
\* TRUE AND no actor has pendingAbort AND abortObserved is FALSE). The bug execution reaches a
\* state where: abort was issued during a Stepup window, BugMode dropped it, then InitCancelSource
\* fired and no pendingAbort exists -- so this conjunction holds, which violates the invariant.
AbortAlwaysHandled ==
    ~(  /\ abortRequested
        /\ ~abortObserved
        /\ \A a \in Actors : hasCancelSource[a]
        /\ \A a \in Actors : ~pendingAbort[a])

\* If the coordinator reached CoordDone, no abort can have been observed before commit. (An abort
\* that lands after CoordDone is a no-op; what we forbid is an observed abort that nonetheless
\* gets bypassed by the commit path.) Encoded as: committed implies abortObserved was reached
\* either after Commit or never. Since Commit guards on ~abortRequested, the only path to
\* committed=TRUE with abortObserved=TRUE is AbortRequest firing after Commit -- a no-op for
\* correctness.
NoCommitAfterAbort ==
    committed /\ abortObserved => coordState = CoordDone

\* If `pendingAbort[a]' is set, then `a' must be either not-primary or in the init gap. Equivalent
\* statement: no pendingAbort ever sits queued on an actor that already has its CancellationSource
\* (such an abort would have been dispatched directly). This catches the buggy implementation that
\* would route an abort to pendingAbort even after the source is ready.
EveryStepupInitializesCancelSource ==
    \A a \in Actors :
        pendingAbort[a] => ~hasCancelSource[a]

\* The coordinator's phase is monotone non-decreasing on the success path: once committed, no
\* primary can be observed in an earlier phase.
CoordMonotoneOnSuccess ==
    committed => coordState = CoordDone

\* Donors and recipients do not move past coordinator phase. If the coordinator is in CoordCloning
\* a recipient cannot already be in RecipApplying. Only checked when the coordinator is on the
\* main flow path (in CoordOrder); CoordAborting is excluded because participants may legitimately
\* still be advancing concurrently with the coordinator's abort transition.
ParticipantsRespectCoordinator ==
    coordState \in { CoordOrder[i] : i \in 1..Len(CoordOrder) } =>
    /\ \A r \in Recipients :
        recipState[r] = RecipApplying
            => CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordApplying)
    /\ \A r \in Recipients :
        recipState[r] = RecipStrictConsistency
            => CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordBlockingWrites)
    /\ \A d \in Donors :
        donorState[d] = DonorDonatingOplog
            => CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordApplying)
    /\ \A d \in Donors :
        donorState[d] = DonorBlockingWrites
            => CoordPhaseIdx(coordState) >= CoordPhaseIdx(CoordBlockingWrites)

\* hasCancelSource is FALSE whenever primary is FALSE.
StepdownDestroysCancelSource ==
    \A a \in Actors : ~primary[a] => ~hasCancelSource[a]

====================================================================================================
