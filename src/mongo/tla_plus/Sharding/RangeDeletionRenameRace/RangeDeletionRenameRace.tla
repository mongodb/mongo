
\* Copyright 2025 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE RangeDeletionRenameRace ---------------------------
(**************************************************************************************************)
(* This specification models the race condition between a collection rename and the range deleter *)
(* service on a single shard. It corresponds to SERVER-114326 (root cause; SERVER-113667 shipped a *)
(* symptom-level skip when the CSR and the RangeDeletionTask UUIDs disagree).                     *)
(*                                                                                                *)
(* The bug is a TOCTOU window between two oplog entries emitted by the rename coordinator:        *)
(*    (1) the update to config.rangeDeletions that rewrites every pending task's collectionUUID   *)
(*        from the old (pre-rename) UUID to the new (post-rename) UUID, and                       *)
(*    (2) the metadata refresh that causes FilteringMetadataClearer to drop the cached            *)
(*        CollectionShardingRuntime entry for the renamed namespace.                              *)
(*                                                                                                *)
(* Empirically the gap between (1) and (2) has been observed at ~300ms, long enough for the       *)
(* RangeDeleterServiceOpObserver::onUpdate hook to fire on (1) and read a RangeDeletionTask       *)
(* stamped with the new UUID while the metadataTracker held by the CSR is still pinned to the    *)
(* old UUID. invalidateRangePreservers() is then called with mismatching UUIDs, which used to     *)
(* invalidate the wrong range preservers; SERVER-113667 short-circuits that path. The deeper      *)
(* invariant -- that the range deleter never observes a (task, metadata) pair with disjoint       *)
(* UUIDs -- is what this spec captures.                                                           *)
(*                                                                                                *)
(* Threads modelled:                                                                              *)
(*   - RenameCoordinator: commits the rename in two oplog entries: COMMIT_RENAME then            *)
(*     CLEAR_METADATA. The two entries land in the oplog in order but the metadata clearer must  *)
(*     still observe and apply the second entry before the cached metadata is dropped.            *)
(*   - RangeDeleterOpObserver: fires onUpdate when (1) lands. Reads the updated task plus the    *)
(*     currently-cached metadata UUID. Decides whether to invalidate range preservers based on    *)
(*     the (task.uuid, metadata.uuid) pair.                                                       *)
(*   - FilteringMetadataClearer: applies (2). Replaces the cached metadata UUID with the new     *)
(*     post-rename UUID (modelled as a clear-then-refresh).                                       *)
(*                                                                                                *)
(* RenameCoordinator transitions:                                                                 *)
(*   - INIT -> RENAME_COMMITTED -> METADATA_CLEARED -> DONE                                       *)
(*                                                                                                *)
(* RangeDeleterOpObserver transitions:                                                            *)
(*   - INIT -> TASK_READ -> METADATA_READ -> DECIDED                                              *)
(*                                                                                                *)
(* The bug is reproduced by toggling AllowCommitBeforeMetadataClear. When TRUE, the op-observer  *)
(* may fire its read-task and read-metadata steps in the gap between RENAME_COMMITTED and        *)
(* METADATA_CLEARED. When FALSE, the spec models the corrected ordering: the metadata clear is    *)
(* sequenced before any onUpdate-triggered observer reads, restoring the safety invariant.        *)
(**************************************************************************************************)

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    OldUUID,                                \* UUID of the collection before rename
    NewUUID,                                \* UUID of the collection after rename
    AllowCommitBeforeMetadataClear          \* Bug toggle: TRUE = reproduce the race

ASSUME OldUUID # NewUUID

(* Modelled UUIDs. NoUUID is used to represent "no metadata cached" (post-clear, pre-refresh). *)
NoUUID == "NoUUID"
UUIDs == {OldUUID, NewUUID, NoUUID}

(* Rename coordinator states. *)
RenameStates == {"INIT", "RENAME_COMMITTED", "METADATA_CLEARED", "DONE"}

(* Op-observer states. *)
ObserverStates == {"INIT", "TASK_READ", "METADATA_READ", "DECIDED"}

(* Range deletion task as stored in config.rangeDeletions. The task's collectionUuid field is    *)
(* mutated in place by the rename coordinator at RENAME_COMMITTED.                               *)
VARIABLES
    taskUUID,                               \* UUID stamped on the RangeDeletionTask document
    metadataUUID,                           \* UUID held by the CSR metadataTracker
    renameState,                            \* Rename coordinator's state machine
    observerState,                          \* Op-observer's state machine
    observerTaskRead,                       \* Snapshot of taskUUID at TASK_READ
    observerMetadataRead,                   \* Snapshot of metadataUUID at METADATA_READ
    invalidationFired,                      \* TRUE iff invalidateRangePreservers was called
    invalidationMismatch                    \* TRUE iff invalidation fired with task.uuid # metadata.uuid

vars == <<taskUUID, metadataUUID, renameState, observerState,
          observerTaskRead, observerMetadataRead,
          invalidationFired, invalidationMismatch>>

Init ==
    /\ taskUUID = OldUUID
    /\ metadataUUID = OldUUID
    /\ renameState = "INIT"
    /\ observerState = "INIT"
    /\ observerTaskRead = NoUUID
    /\ observerMetadataRead = NoUUID
    /\ invalidationFired = FALSE
    /\ invalidationMismatch = FALSE

(**************************************************************************************************)
(* Type invariant.                                                                                 *)
(**************************************************************************************************)

TypeOK ==
    /\ taskUUID \in UUIDs
    /\ metadataUUID \in UUIDs
    /\ renameState \in RenameStates
    /\ observerState \in ObserverStates
    /\ observerTaskRead \in UUIDs
    /\ observerMetadataRead \in UUIDs
    /\ invalidationFired \in BOOLEAN
    /\ invalidationMismatch \in BOOLEAN

(**************************************************************************************************)
(* Actions: RenameCoordinator.                                                                     *)
(**************************************************************************************************)

COMMIT_RENAME ==
(* The rename coordinator commits the rename. The first oplog entry rewrites every               *)
(* RangeDeletionTask document's collectionUuid from OldUUID to NewUUID.                          *)
    /\ renameState = "INIT"
    /\ renameState' = "RENAME_COMMITTED"
    /\ taskUUID' = NewUUID
    /\ UNCHANGED <<metadataUUID, observerState, observerTaskRead, observerMetadataRead,
                   invalidationFired, invalidationMismatch>>

CLEAR_METADATA ==
(* FilteringMetadataClearer applies the second oplog entry. The cached metadata UUID is dropped  *)
(* and replaced with the post-rename UUID. In production this is itself a two-step               *)
(* clear-then-refresh, but for the purpose of modelling the rename / range-deleter race a       *)
(* single atomic transition is sufficient: what matters is whether this transition is sequenced *)
(* relative to the op-observer's reads, not its internal structure.                              *)
    /\ renameState = "RENAME_COMMITTED"
    /\ renameState' = "METADATA_CLEARED"
    /\ metadataUUID' = NewUUID
    /\ UNCHANGED <<taskUUID, observerState, observerTaskRead, observerMetadataRead,
                   invalidationFired, invalidationMismatch>>

RENAME_DONE ==
    /\ renameState = "METADATA_CLEARED"
    /\ renameState' = "DONE"
    /\ UNCHANGED <<taskUUID, metadataUUID, observerState, observerTaskRead, observerMetadataRead,
                   invalidationFired, invalidationMismatch>>

(**************************************************************************************************)
(* Actions: RangeDeleterServiceOpObserver.                                                         *)
(*                                                                                                *)
(* The op-observer fires onUpdate when the RangeDeletionTask document is rewritten by the         *)
(* coordinator. It reads the task body and the cached CSR metadata, then decides whether to       *)
(* call invalidateRangePreservers based on the (task.uuid, metadata.uuid) pair.                   *)
(**************************************************************************************************)

OBS_READ_TASK ==
(* onUpdate fires after the rangeDeletions document update lands. The observer reads the         *)
(* current task body.                                                                            *)
(*                                                                                                *)
(* The bug toggle controls precisely the ordering between the COMMIT_RENAME oplog entry and the  *)
(* onUpdate hook reading the cached metadata. When AllowCommitBeforeMetadataClear is TRUE the   *)
(* observer is permitted to fire as soon as the rename has committed, which is the production    *)
(* behaviour described in SERVER-114326 -- the onUpdate hook fires off the back of the           *)
(* rangeDeletions update and may race the FilteringMetadataClearer. When the toggle is FALSE the *)
(* fix is in place: the observer is gated until renameState reaches METADATA_CLEARED, so the     *)
(* (task, metadata) snapshots cannot diverge.                                                    *)
    /\ observerState = "INIT"
    /\ \/ /\ AllowCommitBeforeMetadataClear
          /\ renameState \in {"RENAME_COMMITTED", "METADATA_CLEARED", "DONE"}
       \/ /\ ~AllowCommitBeforeMetadataClear
          /\ renameState \in {"METADATA_CLEARED", "DONE"}
    /\ observerState' = "TASK_READ"
    /\ observerTaskRead' = taskUUID
    /\ UNCHANGED <<taskUUID, metadataUUID, renameState, observerMetadataRead,
                   invalidationFired, invalidationMismatch>>

OBS_READ_METADATA ==
(* Observer reads the cached metadata UUID from the CSR. In the buggy interleaving this read    *)
(* returns OldUUID even though the task already carries NewUUID.                                  *)
    /\ observerState = "TASK_READ"
    /\ observerState' = "METADATA_READ"
    /\ observerMetadataRead' = metadataUUID
    /\ UNCHANGED <<taskUUID, metadataUUID, renameState, observerTaskRead,
                   invalidationFired, invalidationMismatch>>

OBS_DECIDE ==
(* The observer decides whether to invoke invalidateRangePreservers. The decision is made on    *)
(* the snapshot it just read. The bug is that invalidation may fire with mismatched UUIDs.       *)
    /\ observerState = "METADATA_READ"
    /\ observerState' = "DECIDED"
    /\ invalidationFired' = TRUE
    /\ invalidationMismatch' = (observerTaskRead # observerMetadataRead)
    /\ UNCHANGED <<taskUUID, metadataUUID, renameState, observerTaskRead, observerMetadataRead>>

(**************************************************************************************************)
(* Termination.                                                                                    *)
(**************************************************************************************************)

TERMINATING ==
    /\ renameState = "DONE"
    /\ observerState = "DECIDED"
    /\ UNCHANGED vars

Next ==
    \/ COMMIT_RENAME
    \/ CLEAR_METADATA
    \/ RENAME_DONE
    \/ OBS_READ_TASK
    \/ OBS_READ_METADATA
    \/ OBS_DECIDE
    \/ TERMINATING

Fairness ==
    /\ WF_vars(COMMIT_RENAME)
    /\ WF_vars(CLEAR_METADATA)
    /\ WF_vars(RENAME_DONE)
    /\ WF_vars(OBS_READ_TASK)
    /\ WF_vars(OBS_READ_METADATA)
    /\ WF_vars(OBS_DECIDE)
    /\ WF_vars(TERMINATING)

Spec == Init /\ [][Next]_vars /\ Fairness

(**************************************************************************************************)
(* Safety properties.                                                                              *)
(*                                                                                                *)
(* The headline invariant captures the root cause of SERVER-114326: at any moment the range      *)
(* deleter observes a RangeDeletionTask and the CSR metadata together, those two UUIDs must       *)
(* agree. The action OBS_DECIDE writes invalidationMismatch from the local snapshots, so the     *)
(* invariant inspects those rather than the live values.                                          *)
(**************************************************************************************************)

RangeDeleterMetadataMatchesCollectionUUID ==
    invalidationMismatch = FALSE

(* A weaker invariant: whenever the observer has finished its reads, the snapshots must agree   *)
(* with the global state. Useful for spotting incoherent interleavings even when invalidation    *)
(* itself was skipped.                                                                            *)
ObserverSnapshotsCoherent ==
    (observerState = "DECIDED") =>
        (observerTaskRead = observerMetadataRead)

(* Cleanup invariant: by the time the rename coordinator declares DONE, the cached metadata     *)
(* UUID must equal the task UUID. Without this, a later range deleter wakeup would still see   *)
(* a stale CSR entry.                                                                            *)
PostRenameMetadataIsFresh ==
    (renameState = "DONE") => (metadataUUID = taskUUID)

(**************************************************************************************************)
(* Liveness properties.                                                                            *)
(**************************************************************************************************)

EventuallyRenameCompletes == <>(renameState = "DONE")
EventuallyObserverDecides == <>(observerState = "DECIDED")
EventuallyMetadataIsFresh == <>(metadataUUID = NewUUID)

Termination ==
    /\ <>(renameState = "DONE")
    /\ <>(observerState = "DECIDED")

====================================================================================================
