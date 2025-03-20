
\* Copyright 2025 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------------- MODULE RangeDeletionsSecondaryNodes ------------------------------
(**************************************************************************************************)
(* This specification describes the application of a Range Deletion on a secondary node that runs *)
(* concurrently with a query that is going to advance its storage snapshot.                       *)
(* The main goal is to verify that the query gets killed whenever its snapshot advancement is     *)
(* going to include the Range Deletion.                                                           *)
(*                                                                                                *)
(* We are assuming that:                                                                          *)
(*    - The query that is being modeled in this spec targets a sharded collection and runs on a   *)
(*      secondary node.                                                                           *)
(*    - There is a pending Range Deletion that will be executed during the modelling of this spec.*)
(*    - We're just modeling a single shard, a single query and a single collection.                    *)
(*    - The query started before the migration that caused the Range Deletion that is being       *)
(*      modeled in this spec. Therefore, the RangePreserver being held by this query will get     *)
(*      invalidated once the Range Deletion gets executed.                                        *)
(*                                                                                                *)
(* Query possible transitions:                                                                    *)
(*  - RESTORE_START -> SNAPSHOT_ADVANCED -> DONE_OK                                               *)
(*  - RESTORE_START -> SNAPSHOT_ADVANCED -> KILLED                                                *)
(*                                                                                                *)
(* When a Range Deletion is executed, first, there is a write on `config.rangeDeletions` to       *)
(* signal the upcoming Range Deletion to all secondary nodes. Then, the range deletion is         *)
(* executed right away.                                                                           *)
(* Therefore, we're going to model two Oplog Applier threads (one for the Range Deletion          *)
(* signaling and another for the Range Deletion itself), and we'll assume they run on the same    *)
(* batch to exemplify the most pessimistic scenario. If both operations run on different batches  *)
(* we can make sure that the invalidation of the Range Preserver will precede the actual Range    *)
(* Deletion operation. However, if they run on the same batch, both operations will concurrently. *)
(*                                                                                                *)
(* OpApplier possible transitions:                                                                *)
(*  - INIT -> UPDATED -> COMMITTED                                                                *)
(*                                                                                                *)
(* Oplog batch possible transitions:                                                              *)
(*  - ONGOING -> COMMITTED                                                                        *)
(*                                                                                                *)
EXTENDS Integers, FiniteSets, Sequences, TLC

(* - DOCS will contain the documents on the collection.                                           *)
(* - DOC_REMOVED_BY_RANGE_DELETER is the doc that will be removed once the Range Deletion gets    *)
(*   executed.                                                                                    *)
DOCS == {1,2}
DOC_REMOVED_BY_RANGE_DELETER == 2
ASSUME DOC_REMOVED_BY_RANGE_DELETER \in DOCS

(* Define the two types of oplog applier threads.                                                 *)
OpApplierType == {"RangeDeletionDeleteOp", "RangeDeletionAboutToStartSignal"}

(* Define the variables related to the oplog applier threads.                                     *)
VARIABLES
    opApplierState,        \* opApplierState[opType] is the state of an ongoing op applier thread
    batchState             \* Is the state of the batch containing both oplog appliers

(* Define the variables related to the query thread.                                              *)
VARIABLES
    queryState,            \* Is the state of the ongoing query
    querySnapshot,         \* Set of docs that the query will see.
    rangePreserverIsValid  \* Is the state of the RangePreserver attached to the ongoing query

(* Define the variables related to the collection and its metadata.                               *)
VARIABLES
    lastAppliedSnapshot    \* Docs that will be read by any operation that advances its storage snapshot


vars == <<opApplierState, batchState, queryState, querySnapshot, rangePreserverIsValid, lastAppliedSnapshot>>

Init == 
    /\ opApplierState = [opType \in OpApplierType |-> "INIT"]
    /\ batchState = "ONGOING"
    /\ queryState = "RESTORE_START"
    /\ querySnapshot = {}
    /\ rangePreserverIsValid = TRUE
    /\ lastAppliedSnapshot = DOCS

(**************************************************************************************************)
(* Safety properties.                                                                             *)
(**************************************************************************************************)

TypeOK ==
    /\ opApplierState \in [OpApplierType -> {"INIT", "UPDATED", "COMMITTED"}]
    /\ batchState \in {"ONGOING", "COMMITTED"}
    /\ queryState \in {"RESTORE_START", "SNAPSHOT_ADVANCED", "DONE_OK", "KILLED"}
    /\ querySnapshot \subseteq DOCS
    /\ rangePreserverIsValid \in {TRUE,FALSE}
    /\ lastAppliedSnapshot \subseteq DOCS

(**************************************************************************************************)
(* Actions.                                                                                       *)
(**************************************************************************************************)

OP_UPDATED(opType) == 
(* An op applier thread reaches the UPDATED state. If it's the RangeDeletionSignaling thread,     *)
(* mark the Range Preserver as invalid.                                                           *)
    /\ opApplierState[opType] = "INIT"
    /\ opApplierState' = [opApplierState EXCEPT ![opType] = "UPDATED"]
    /\ IF opType = "RangeDeletionAboutToStartSignal" THEN rangePreserverIsValid' = FALSE ELSE rangePreserverIsValid' = rangePreserverIsValid 
    /\ UNCHANGED <<queryState, lastAppliedSnapshot, querySnapshot, batchState>>

OP_COMMITTED(opType) == 
(* An op applier thread reaches the COMMITTED state.                                              *)
    /\ opApplierState[opType] = "UPDATED"
    /\ opApplierState' = [opApplierState EXCEPT ![opType] = "COMMITTED"]
    /\ UNCHANGED <<queryState, lastAppliedSnapshot, rangePreserverIsValid, querySnapshot, batchState>>
              
BATCH_COMMITTED == 
(* Once all the op applier threads are in committed state, the latestApplied timestamp will get   *)
(* incremented. Consequently, the range deletion will be visibe to anyone advancing its storage   *)
(* snapshot.                                                                                      *)
    /\ batchState = "ONGOING"
    /\ \A opType \in OpApplierType : opApplierState[opType] = "COMMITTED"
    /\ batchState' = "COMMITTED"
    /\ lastAppliedSnapshot' = lastAppliedSnapshot \ {DOC_REMOVED_BY_RANGE_DELETER}
    /\ UNCHANGED <<opApplierState, queryState, rangePreserverIsValid, querySnapshot>>

Q_ADVANCE_SNAPSHOT ==
(* Advance the snapshot of the query and store a snapshot of lastAppliedSnapshot on the           *)
(* `querySnapshot` variable.                                                                      *)
    /\ queryState = "RESTORE_START"
    /\ queryState' = "SNAPSHOT_ADVANCED"
    /\ querySnapshot' = lastAppliedSnapshot
    /\ UNCHANGED <<opApplierState, lastAppliedSnapshot, rangePreserverIsValid, batchState>>

Q_KILLED ==
(* Kill the query if the Range Preserver has been invalidated.                                    *)
    /\ queryState = "SNAPSHOT_ADVANCED"
    /\ rangePreserverIsValid = FALSE
    /\ queryState' = "KILLED"
    /\ UNCHANGED <<opApplierState, lastAppliedSnapshot, rangePreserverIsValid, querySnapshot, batchState>>

Q_PROCEED ==
(* The query should proceed if the Range Preserver hasn't been invalidated.                       *)
    /\ queryState = "SNAPSHOT_ADVANCED"
    /\ rangePreserverIsValid = TRUE
    /\ queryState' = "DONE_OK"
    /\ UNCHANGED <<opApplierState, lastAppliedSnapshot, rangePreserverIsValid, querySnapshot, batchState>>

TERMINATING ==
    /\ queryState \in {"DONE_OK", "KILLED"}
    /\ batchState = "COMMITTED"
    /\ UNCHANGED vars

Next == 
    \/ Q_ADVANCE_SNAPSHOT
    \/ Q_KILLED
    \/ Q_PROCEED
    \/ \E opType \in OpApplierType : OP_UPDATED(opType)
    \/ \E opType \in OpApplierType : OP_COMMITTED(opType)
    \/ BATCH_COMMITTED
    \/ TERMINATING

\* Ensure all actions are executed when they are enabled: *\
Fairness ==
    /\ WF_vars(Q_ADVANCE_SNAPSHOT)
    /\ WF_vars(Q_KILLED)
    /\ WF_vars(Q_PROCEED)
    /\ WF_vars(\E opType \in OpApplierType : OP_UPDATED(opType))
    /\ WF_vars(\E opType \in OpApplierType : OP_COMMITTED(opType))
    /\ WF_vars(BATCH_COMMITTED)
    /\ WF_vars(TERMINATING)

Spec == Init /\ [][Next]_vars /\ Fairness

(**************************************************************************************************)
(* Liveness properties.                                                                           *)
(**************************************************************************************************)

QueryShouldReadAllDocs == [] (queryState = "DONE_OK" => querySnapshot = DOCS)

RangePreserverShouldBeInvalidatedBeforeRangeDeletionGetsVisible == [] (DOC_REMOVED_BY_RANGE_DELETER \notin lastAppliedSnapshot => rangePreserverIsValid = FALSE)
RangeDeletionIsExecuted == <> (DOC_REMOVED_BY_RANGE_DELETER \notin lastAppliedSnapshot)
RangePreserverIsInvalidated == <> (rangePreserverIsValid = FALSE)
Termination == <>(queryState \in {"KILLED", "DONE_OK"})
    /\ <>(\A opType \in OpApplierType : opApplierState[opType] = "COMMITTED")
    /\ <>(batchState = "COMMITTED")

(* Uncomment and add this property on .cfg file to verify the hypothesis that a query may get     *)
(* even if the deleted range is still in `querySnapshot`.                                         *)
\* QueryMayGetKilledWhenRangeDeletionWasNotAppliedYet == [] (queryState = "KILLED" => DOC_REMOVED_BY_RANGE_DELETER \in querySnapshot)

====================================================================================================

