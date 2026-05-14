\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------ MODULE ConfigServerTransitionOrphan ---------------------------------------
\* This specification models the interleaving between two concurrent activities on a
\* config-server-shard (the cluster member that hosts both the config server and a data shard, see
\* SERVER-103990):
\*
\*   (a) A chunk migration committing on the donor. The donor:
\*       1. persists the commit decision and registers an on-disk range-deletion task document in
\*          `config.rangeDeletions` with a `pending: true` field.
\*       2. registers an in-memory range-deletion task with the RangeDeleterService in the
\*          `Pending` state.
\*       3. invokes `markAsReadyRangeDeletionTaskLocally`, which `$unset`s the `pending` field on
\*          the on-disk doc. The OpObserver for that update calls `clearPending()` on the
\*          in-memory task, releasing the deletion chain so it can run.
\*
\*   (b) A `transitionToDedicatedConfigServer` command running concurrently. As part of the
\*       transition the config-server-shard drops `config.rangeDeletions` (SERVER-103990).
\*
\* The bug (SERVER-125663): if the drop in (b.4) is interleaved between (a.2) and (a.3), then the
\* update in `markAsReadyRangeDeletionTaskLocally` raises `NoMatchingDocument`. The exception is
\* swallowed by the catch-block in `range_deletion_util.cpp`. `clearPending()` is never called on
\* the in-memory task, so the deletion chain stalls forever and the orphan documents on the former
\* config-server-shard accumulate. A secondary effect: if the migration was issued with
\* `waitForDelete = true` it blocks on the never-completing future and only unwinds when the op
\* context is killed externally.
\*
\* The spec models the four steps as four actions and uses a CONSTANT flag `AllowDropBeforeMarkReady`
\* to selectively enable the racing interleaving. With the flag set to FALSE the deletion-chain
\* invariant `EveryPendingRangeEventuallyCleared` holds. With the flag set to TRUE TLC produces a
\* counterexample exhibiting the SERVER-125663 hazard, demonstrating the falsifier landed where the
\* bug actually lives.
\*
\* The spec deliberately reduces the surface area of MoveRange.tla: it does not model the routing
\* protocol, recipient, or ownership filters. Those are covered upstream by MoveRange.tla. This spec
\* covers exclusively the donor-side range-deletion-readiness handoff and the transition drop.
\*
\* To run the model-checker, from `src/mongo/tla_plus`:
\*     ./model-check.sh Sharding/ConfigServerTransitionOrphan

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Ranges,                     \* Set of ranges that may be migrated off the config-server-shard.
    AllowDropBeforeMarkReady    \* Bug toggle. When TRUE, the transition may drop
                                \* `config.rangeDeletions` after a task has been registered
                                \* in-memory but before `markAsReadyRangeDeletionTaskLocally`
                                \* has run, reproducing SERVER-125663.

ASSUME Cardinality(Ranges) > 0
ASSUME AllowDropBeforeMarkReady \in BOOLEAN

\* Per-range states for the donor's range-deletion lifecycle.
\* Possible transitions (green path):
\*   Unset -> Committed -> Registered -> Ready -> Cleared
\* Bug path (only reachable when `AllowDropBeforeMarkReady = TRUE`):
\*   Unset -> Committed -> Registered -> StuckPending   (terminal)
RangeUnset       == "unset"        \* No migration in flight for this range.
RangeCommitted   == "committed"    \* Donor persisted a `config.rangeDeletions` doc with `pending`.
RangeRegistered  == "registered"   \* In-memory task registered in `Pending` state.
RangeReady       == "ready"        \* `pending` field $unset; OpObserver fired `clearPending()`.
RangeCleared     == "cleared"      \* Deletion chain ran to completion. Orphans gone.
RangeStuckPending == "stuckPending" \* Bug terminal: in-memory task forever in `Pending` state.

RangeStates == {RangeUnset, RangeCommitted, RangeRegistered, RangeReady, RangeCleared,
                RangeStuckPending}

\* State of `config.rangeDeletions` on the config-server-shard.
RangeDeletionsCollPresent == "present"
RangeDeletionsCollDropped == "dropped"
RangeDeletionsCollStates  == {RangeDeletionsCollPresent, RangeDeletionsCollDropped}

\* State of the transition itself.
\* Possible transitions:
\*   NotStarted -> Draining -> DroppingRangeDeletions -> Done
TransitionNotStarted             == "notStarted"
TransitionDraining               == "draining"
TransitionDroppingRangeDeletions == "droppingRangeDeletions"
TransitionDone                   == "done"
TransitionStates == {TransitionNotStarted, TransitionDraining,
                     TransitionDroppingRangeDeletions, TransitionDone}

VARIABLES
    rangeState,             \* rangeState[r] in RangeStates -- per-range lifecycle.
    onDiskPending,          \* onDiskPending[r] in BOOLEAN -- the on-disk `pending` field. Only
                            \* meaningful while rangeDeletionsColl = present.
    rangeDeletionsColl,     \* Current state of `config.rangeDeletions` on the config-server-shard.
    transitionState         \* Current state of `transitionToDedicatedConfigServer`.

vars == <<rangeState, onDiskPending, rangeDeletionsColl, transitionState>>

Init ==
    /\ rangeState         = [r \in Ranges |-> RangeUnset]
    /\ onDiskPending      = [r \in Ranges |-> FALSE]
    /\ rangeDeletionsColl = RangeDeletionsCollPresent
    /\ transitionState    = TransitionNotStarted

(* --------------------------------------------------------------------------------------------- *)
(* Migration-coordinator actions                                                                 *)
(* --------------------------------------------------------------------------------------------- *)

\* Step 1: the donor persists the commit decision and inserts a `config.rangeDeletions` doc with
\* `pending: true`. Modelled by `persistCommitDecision` followed by the insertion of the
\* `RangeDeletionTask` document in `migration_coordinator.cpp::completeMigration`.
\*
\* We require the on-disk collection to still be present. If the transition has already dropped
\* `config.rangeDeletions` before the migration reaches this step the migration coordinator either
\* hits a different error path or never starts; either way it is not the SERVER-125663 hazard. We
\* therefore exclude that interleaving from the model and document it explicitly.
CommitMigration(r) ==
    /\ rangeState[r] = RangeUnset
    /\ rangeDeletionsColl = RangeDeletionsCollPresent
    /\ rangeState'    = [rangeState    EXCEPT ![r] = RangeCommitted]
    /\ onDiskPending' = [onDiskPending EXCEPT ![r] = TRUE]
    /\ UNCHANGED <<rangeDeletionsColl, transitionState>>

\* Step 2: register the in-memory range-deletion task with `RangeDeleterService::registerTask` in
\* `TaskPending::kPending`. This sets up the completion future that downstream code awaits.
RegisterInMemoryTask(r) ==
    /\ rangeState[r] = RangeCommitted
    /\ rangeState'   = [rangeState EXCEPT ![r] = RangeRegistered]
    /\ UNCHANGED <<onDiskPending, rangeDeletionsColl, transitionState>>

\* Step 3 (green path): `markAsReadyRangeDeletionTaskLocally` runs while `config.rangeDeletions` is
\* still present. The `$unset` of the `pending` field succeeds; the OpObserver fires and calls
\* `clearPending()` on the in-memory task. The deletion chain becomes runnable.
MarkAsReady_Success(r) ==
    /\ rangeState[r] = RangeRegistered
    /\ rangeDeletionsColl = RangeDeletionsCollPresent
    /\ onDiskPending[r] = TRUE
    /\ rangeState'    = [rangeState    EXCEPT ![r] = RangeReady]
    /\ onDiskPending' = [onDiskPending EXCEPT ![r] = FALSE]
    /\ UNCHANGED <<rangeDeletionsColl, transitionState>>

\* Step 3 (bug path): if `config.rangeDeletions` has been dropped while the in-memory task is in
\* `Registered`, the update raises `NoMatchingDocument`. The catch-block at
\* `range_deletion_util.cpp:759` swallows the exception. `clearPending()` is never called and the
\* in-memory task is wedged in the `Pending` state forever.
MarkAsReady_NoMatchingDocument(r) ==
    /\ rangeState[r] = RangeRegistered
    /\ rangeDeletionsColl = RangeDeletionsCollDropped
    /\ rangeState' = [rangeState EXCEPT ![r] = RangeStuckPending]
    /\ UNCHANGED <<onDiskPending, rangeDeletionsColl, transitionState>>

\* Step 4: the deletion chain runs to completion once `clearPending()` has unblocked it. Orphans
\* are removed. This is the only path that satisfies the safety invariant.
RunDeletionChain(r) ==
    /\ rangeState[r] = RangeReady
    /\ rangeState' = [rangeState EXCEPT ![r] = RangeCleared]
    /\ UNCHANGED <<onDiskPending, rangeDeletionsColl, transitionState>>

(* --------------------------------------------------------------------------------------------- *)
(* transitionToDedicatedConfigServer actions                                                     *)
(* --------------------------------------------------------------------------------------------- *)

\* Operator issues `startTransitionToDedicatedConfigServer`.
StartTransition ==
    /\ transitionState = TransitionNotStarted
    /\ transitionState' = TransitionDraining
    /\ UNCHANGED <<rangeState, onDiskPending, rangeDeletionsColl>>

\* The transition reaches the step where it is about to drop `config.rangeDeletions`. Modelled as
\* an explicit pre-state because TLC needs to see a step where the decision is irreversible and the
\* in-memory state hasn't been disturbed.
BeginDropRangeDeletions ==
    /\ transitionState = TransitionDraining
    /\ transitionState' = TransitionDroppingRangeDeletions
    /\ UNCHANGED <<rangeState, onDiskPending, rangeDeletionsColl>>

\* The transition drops `config.rangeDeletions`. The bug toggle restricts whether the drop is
\* allowed to happen with an in-memory task still in `Registered` state.
\*
\* * Green path (`AllowDropBeforeMarkReady = FALSE`): the drop is only permitted after every
\*   in-memory task has been cleared or is back to `Unset`. This models a hypothetical fix in
\*   which the transition coordinator drains in-flight migrations before dropping the collection.
\* * Bug path (`AllowDropBeforeMarkReady = TRUE`): the drop is unconditional, reproducing the
\*   current production behaviour where the transition is not aware of in-flight migrations.
DropRangeDeletions ==
    /\ transitionState = TransitionDroppingRangeDeletions
    /\ rangeDeletionsColl = RangeDeletionsCollPresent
    /\ \/ AllowDropBeforeMarkReady = TRUE
       \/ \A r \in Ranges : rangeState[r] \in {RangeUnset, RangeReady, RangeCleared}
    /\ rangeDeletionsColl' = RangeDeletionsCollDropped
    /\ onDiskPending'      = [r \in Ranges |-> FALSE] \* dropped collection -> no on-disk pending.
    /\ UNCHANGED <<rangeState, transitionState>>

\* Operator commits the transition. Terminal step.
CommitTransition ==
    /\ transitionState = TransitionDroppingRangeDeletions
    /\ rangeDeletionsColl = RangeDeletionsCollDropped
    /\ transitionState' = TransitionDone
    /\ UNCHANGED <<rangeState, onDiskPending, rangeDeletionsColl>>

(* --------------------------------------------------------------------------------------------- *)
(* Next-state relation                                                                           *)
(* --------------------------------------------------------------------------------------------- *)

Next ==
    \/ \E r \in Ranges : CommitMigration(r)
    \/ \E r \in Ranges : RegisterInMemoryTask(r)
    \/ \E r \in Ranges : MarkAsReady_Success(r)
    \/ \E r \in Ranges : MarkAsReady_NoMatchingDocument(r)
    \/ \E r \in Ranges : RunDeletionChain(r)
    \/ StartTransition
    \/ BeginDropRangeDeletions
    \/ DropRangeDeletions
    \/ CommitTransition

Fairness ==
    /\ WF_vars(\E r \in Ranges : CommitMigration(r))
    /\ WF_vars(\E r \in Ranges : RegisterInMemoryTask(r))
    /\ WF_vars(\E r \in Ranges : MarkAsReady_Success(r))
    /\ WF_vars(\E r \in Ranges : MarkAsReady_NoMatchingDocument(r))
    /\ WF_vars(\E r \in Ranges : RunDeletionChain(r))
    /\ WF_vars(StartTransition)
    /\ WF_vars(BeginDropRangeDeletions)
    /\ WF_vars(DropRangeDeletions)
    /\ WF_vars(CommitTransition)

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariant                                                                                 *)
(**************************************************************************************************)

TypeOK ==
    /\ rangeState         \in [Ranges -> RangeStates]
    /\ onDiskPending      \in [Ranges -> BOOLEAN]
    /\ rangeDeletionsColl \in RangeDeletionsCollStates
    /\ transitionState    \in TransitionStates

(**************************************************************************************************)
(* Safety -- the deletion chain that backs orphan cleanup is never permanently wedged.            *)
(**************************************************************************************************)

\* Every range that was committed for migration is either still in flight or has been cleared.
\* Ranges may reach `StuckPending` only transiently; if they remain there forever the in-memory
\* task is permanently in the `Pending` state and orphans accumulate (SERVER-125663).
NoRangePermanentlyStuck == \A r \in Ranges : rangeState[r] # RangeStuckPending

(**************************************************************************************************)
(* Liveness                                                                                       *)
(**************************************************************************************************)

\* The headline correctness property: every range that the donor begins to migrate eventually
\* either reaches `Cleared` (deletion chain ran) or stays at `Unset` (migration never started).
\* SERVER-125663 falsifies this because `StuckPending` is a terminal sink under the bug path.
EveryPendingRangeEventuallyCleared ==
    \A r \in Ranges : []((rangeState[r] = RangeCommitted) => <>(rangeState[r] = RangeCleared))

\* The transition itself eventually completes. Included to make sure the green-path config doesn't
\* trivially satisfy `EveryPendingRangeEventuallyCleared` by simply never running the transition.
TransitionEventuallyCommits == <>(transitionState = TransitionDone)

====================================================================================================
