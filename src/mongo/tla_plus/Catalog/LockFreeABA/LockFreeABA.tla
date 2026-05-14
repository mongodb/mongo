\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/
-------------------------------- MODULE LockFreeABA ----------------------------------------------
\* Formal specification for the lock-free acquisition ABA hazard described in SERVER-76561.
\*
\* Scenario (verbatim from the ticket, expressed as an interleaving of three concurrent operations):
\*   1. Namespace N exists as UNSHARDED, with epoch e0.
\*   2. Reader R receives a request from mongos with attached shard version UNSHARDED.
\*   3. R performs the FIRST sharding-placement check (pre-snapshot): namespace is UNSHARDED, pass.
\*   4. A concurrent writer W1 shards the collection: epoch transitions e0 -> e1, state SHARDED.
\*   5. R opens its lock-free storage snapshot at this point.
\*   6. A concurrent writer W2 drops the collection: epoch -> e2, state DROPPED.
\*   7. A concurrent writer W3 recreates the collection as UNSHARDED: epoch -> e3, state UNSHARDED.
\*   8. R performs the SECOND sharding-placement check (post-snapshot, double-check): the namespace
\*      is again UNSHARDED, matching the attached UNSHARDED shard version. The check passes.
\*   9. R proceeds to read from the snapshot opened in step 5, which captured the SHARDED incarnation,
\*      and returns rows as if the collection were unsharded -- the ABA anomaly.
\*
\* The spec captures the UUID epoch counter, the placement-state ABA, and the placement-equality
\* check that is the load-bearing guard in db_raii.cpp (lines 1082-1105 at the linked commit). It
\* models a *single* namespace because the hazard is intra-namespace; multi-namespace coverage is
\* the province of TxnsCollectionIncarnation.
\*
\* The bug invariant LockFreeReadObservesConsistentIncarnation asserts that a successful
\* lock-free acquisition's snapshot epoch equals the epoch latched by the second placement check.
\* With placement-equality as the only guard, TLC produces a counterexample matching steps 1-9.
\* With epoch-equality as the guard, the invariant holds.
\*
\* To run the model-checker, first edit the constants in MCLockFreeABA.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Catalog/LockFreeABA

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Readers,           \* set of lock-free reader threads
    Writers,           \* set of writer threads performing DDLs
    MAX_EPOCH,         \* cap on epoch counter to bound the state space
    GUARD              \* "placement" (buggy) or "epoch" (fixed)

\* Placement states the namespace can be in.
DROPPED   == "dropped"
UNSHARDED == "unsharded"
SHARDED   == "sharded"

PlacementStates == {DROPPED, UNSHARDED, SHARDED}

\* Special UUID/epoch sentinel meaning "no incarnation".
NO_EPOCH == 0

\* Per-reader phases of a lock-free acquisition.
\* IDLE  -> received request, not yet checking
\* PRE   -> first sharding-placement check passed (pre-snapshot)
\* SNAP  -> snapshot has been opened at the storage layer
\* POST  -> second sharding-placement check passed (post-snapshot, double-check)
\* DONE  -> acquisition resolved (committed or restarted)
ReaderPhases == {"IDLE", "PRE", "SNAP", "POST", "DONE"}

ReaderResult  == {"committed", "restart"}

ASSUME Cardinality(Readers) >= 1
ASSUME Cardinality(Writers) >= 1
ASSUME MAX_EPOCH \in 2..20
ASSUME GUARD \in {"placement", "epoch"}

(***************************************************************************************************)
(* Authoritative catalog state.                                                                    *)
(***************************************************************************************************)
VARIABLE catalogState       \* current placement state of the namespace
VARIABLE catalogEpoch       \* current UUID/incarnation epoch (monotonic)
VARIABLE nextEpoch          \* next epoch to be minted on the next placement-changing DDL

(***************************************************************************************************)
(* Per-reader local state for a lock-free acquisition attempt.                                     *)
(***************************************************************************************************)
VARIABLE rPhase             \* phase the reader is in
VARIABLE rAttachedState     \* placement state attached by mongos to the request
VARIABLE rAttachedEpoch     \* epoch attached by mongos (or NO_EPOCH if UNSHARDED in buggy world)
VARIABLE rPreState          \* placement state observed at PRE check
VARIABLE rPreEpoch          \* epoch observed at PRE check
VARIABLE rSnapState         \* placement state at snapshot open
VARIABLE rSnapEpoch         \* epoch at snapshot open (the incarnation actually read)
VARIABLE rPostState         \* placement state at POST check
VARIABLE rPostEpoch         \* epoch at POST check
VARIABLE rResult            \* "committed" or "restart" once DONE

catalog_vars  == << catalogState, catalogEpoch, nextEpoch >>
reader_vars   == << rPhase, rAttachedState, rAttachedEpoch, rPreState, rPreEpoch,
                    rSnapState, rSnapEpoch, rPostState, rPostEpoch, rResult >>
vars          == << catalog_vars, reader_vars >>

Init ==
    /\ catalogState = UNSHARDED
    /\ catalogEpoch = 1
    /\ nextEpoch    = 2
    /\ rPhase          = [r \in Readers |-> "IDLE"]
    /\ rAttachedState  = [r \in Readers |-> UNSHARDED]
    /\ rAttachedEpoch  = [r \in Readers |-> NO_EPOCH]
    /\ rPreState       = [r \in Readers |-> UNSHARDED]
    /\ rPreEpoch       = [r \in Readers |-> NO_EPOCH]
    /\ rSnapState      = [r \in Readers |-> UNSHARDED]
    /\ rSnapEpoch      = [r \in Readers |-> NO_EPOCH]
    /\ rPostState      = [r \in Readers |-> UNSHARDED]
    /\ rPostEpoch      = [r \in Readers |-> NO_EPOCH]
    /\ rResult         = [r \in Readers |-> "restart"]

(***************************************************************************************************)
(* Writer (DDL) actions.                                                                           *)
(*                                                                                                 *)
(* Each placement-changing DDL bumps the epoch counter monotonically. ShardCollection moves        *)
(* UNSHARDED -> SHARDED; Drop moves either UNSHARDED or SHARDED to DROPPED; CreateUnsharded moves   *)
(* DROPPED -> UNSHARDED. DDLs do not need to coordinate with the lock-free reader -- that is       *)
(* precisely the reason the hazard exists.                                                         *)
(***************************************************************************************************)

ShardCollection ==
    /\ catalogState = UNSHARDED
    /\ nextEpoch <= MAX_EPOCH
    /\ catalogState' = SHARDED
    /\ catalogEpoch' = nextEpoch
    /\ nextEpoch'    = nextEpoch + 1
    /\ UNCHANGED reader_vars

DropCollection ==
    /\ catalogState \in {UNSHARDED, SHARDED}
    /\ nextEpoch <= MAX_EPOCH
    /\ catalogState' = DROPPED
    /\ catalogEpoch' = nextEpoch
    /\ nextEpoch'    = nextEpoch + 1
    /\ UNCHANGED reader_vars

CreateUnsharded ==
    /\ catalogState = DROPPED
    /\ nextEpoch <= MAX_EPOCH
    /\ catalogState' = UNSHARDED
    /\ catalogEpoch' = nextEpoch
    /\ nextEpoch'    = nextEpoch + 1
    /\ UNCHANGED reader_vars

(***************************************************************************************************)
(* Reader (lock-free acquisition) actions.                                                         *)
(*                                                                                                 *)
(* ReceiveRequest    : mongos hands the reader a request with an attached shard version.           *)
(* CheckPlacementPre : first sharding-placement check, before opening snapshot.                    *)
(* OpenSnapshot      : storage-engine snapshot is opened. From here, reads are pinned to the       *)
(*                     incarnation captured in rSnap*.                                             *)
(* CheckPlacementPost: second sharding-placement check, the load-bearing guard against ABA. Either *)
(*                     placement-equality (buggy) or epoch-equality (fixed) is used, controlled by *)
(*                     the GUARD constant.                                                         *)
(***************************************************************************************************)

ReceiveRequest(r) ==
    /\ rPhase[r] = "IDLE"
    /\ \E attState \in PlacementStates :
        /\ rAttachedState' = [rAttachedState EXCEPT ![r] = attState]
        \* In production, mongos attaches an epoch when the namespace is tracked/sharded but the
        \* UNSHARDED shard version carries NO_EPOCH. That asymmetry is precisely what enables ABA
        \* under the placement-equality guard.
        /\ rAttachedEpoch' = [rAttachedEpoch EXCEPT ![r] =
                                IF attState = UNSHARDED THEN NO_EPOCH ELSE catalogEpoch]
    /\ rPhase' = [rPhase EXCEPT ![r] = "PRE"]
    /\ UNCHANGED << catalog_vars, rPreState, rPreEpoch, rSnapState, rSnapEpoch,
                    rPostState, rPostEpoch, rResult >>

\* The first placement check matches attached vs current. If it mismatches, the reader restarts.
CheckPlacementPre(r) ==
    /\ rPhase[r] = "PRE"
    /\ rPreState'  = [rPreState  EXCEPT ![r] = catalogState]
    /\ rPreEpoch'  = [rPreEpoch  EXCEPT ![r] = catalogEpoch]
    /\ IF catalogState = rAttachedState[r]
       THEN /\ rPhase'  = [rPhase  EXCEPT ![r] = "SNAP"]
            /\ rResult' = rResult
       ELSE /\ rPhase'  = [rPhase  EXCEPT ![r] = "DONE"]
            /\ rResult' = [rResult EXCEPT ![r] = "restart"]
    /\ UNCHANGED << catalog_vars, rAttachedState, rAttachedEpoch,
                    rSnapState, rSnapEpoch, rPostState, rPostEpoch >>

\* Open the lock-free snapshot. The snapshot pins the *current* incarnation -- this is the value
\* the reader will return rows from, regardless of what later DDLs do.
OpenSnapshot(r) ==
    /\ rPhase[r] = "SNAP"
    /\ rSnapState' = [rSnapState EXCEPT ![r] = catalogState]
    /\ rSnapEpoch' = [rSnapEpoch EXCEPT ![r] = catalogEpoch]
    /\ rPhase'     = [rPhase     EXCEPT ![r] = "POST"]
    /\ UNCHANGED << catalog_vars, rAttachedState, rAttachedEpoch,
                    rPreState, rPreEpoch, rPostState, rPostEpoch, rResult >>

\* The post-snapshot guard. Under GUARD="placement" this is the buggy world from db_raii.cpp:
\* we only confirm the placement state still matches the attached shard version. Under
\* GUARD="epoch" the guard additionally requires the namespace epoch to be the same one captured
\* in the snapshot -- the ABA-defeating fix.
CheckPlacementPost(r) ==
    /\ rPhase[r] = "POST"
    /\ rPostState' = [rPostState EXCEPT ![r] = catalogState]
    /\ rPostEpoch' = [rPostEpoch EXCEPT ![r] = catalogEpoch]
    /\ LET placementOk == catalogState = rAttachedState[r]
           epochOk     == catalogEpoch = rSnapEpoch[r]
           accept      == IF GUARD = "placement"
                          THEN placementOk
                          ELSE placementOk /\ epochOk
       IN /\ rPhase'  = [rPhase  EXCEPT ![r] = "DONE"]
          /\ rResult' = [rResult EXCEPT ![r] = IF accept THEN "committed" ELSE "restart"]
    /\ UNCHANGED << catalog_vars, rAttachedState, rAttachedEpoch,
                    rPreState, rPreEpoch, rSnapState, rSnapEpoch >>

ReaderStep(r) ==
    \/ ReceiveRequest(r)
    \/ CheckPlacementPre(r)
    \/ OpenSnapshot(r)
    \/ CheckPlacementPost(r)

Next ==
    \/ \E w \in Writers : ShardCollection
    \/ \E w \in Writers : DropCollection
    \/ \E w \in Writers : CreateUnsharded
    \/ \E r \in Readers : ReaderStep(r)
    \* Allow stuttering when every reader is DONE and no more DDLs fit under MAX_EPOCH.
    \/ ( (\A r \in Readers : rPhase[r] = "DONE") /\ nextEpoch > MAX_EPOCH /\ UNCHANGED vars )

Fairness ==
    /\ \A r \in Readers : WF_vars(ReaderStep(r))

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(***************************************************************************************************)
(* Type invariants.                                                                                *)
(***************************************************************************************************)

TypeOK ==
    /\ catalogState \in PlacementStates
    /\ catalogEpoch \in 0..MAX_EPOCH
    /\ nextEpoch    \in 1..(MAX_EPOCH+1)
    /\ \A r \in Readers :
        /\ rPhase[r]         \in ReaderPhases
        /\ rAttachedState[r] \in PlacementStates
        /\ rAttachedEpoch[r] \in 0..MAX_EPOCH
        /\ rPreState[r]      \in PlacementStates
        /\ rSnapState[r]     \in PlacementStates
        /\ rPostState[r]     \in PlacementStates
        /\ rResult[r]        \in ReaderResult

EpochMonotonic == catalogEpoch <= nextEpoch - 1

(***************************************************************************************************)
(* Correctness Properties.                                                                         *)
(*                                                                                                 *)
(* LockFreeReadObservesConsistentIncarnation: the load-bearing invariant. If a reader returned     *)
(* "committed" then the incarnation observed in its snapshot is identical to the incarnation       *)
(* observed by the post-snapshot guard. With GUARD="placement" TLC produces a counterexample        *)
(* mirroring steps 1-9 from the spec header. With GUARD="epoch" the invariant holds.               *)
(***************************************************************************************************)

LockFreeReadObservesConsistentIncarnation ==
    \A r \in Readers :
        (rPhase[r] = "DONE" /\ rResult[r] = "committed")
            => rSnapEpoch[r] = rPostEpoch[r]

\* A stronger sibling invariant: a committed read attached UNSHARDED iff the snapshot was taken
\* against an UNSHARDED incarnation. This is the property the user-visible bug violates -- the
\* router thinks it is reading an unsharded collection but the snapshot held a sharded one.
AttachedStateMatchesSnapshotState ==
    \A r \in Readers :
        (rPhase[r] = "DONE" /\ rResult[r] = "committed")
            => rAttachedState[r] = rSnapState[r]

\* Liveness: every reader eventually finishes.
ReadersEventuallyDone == <>[] \A r \in Readers : rPhase[r] = "DONE"

(***************************************************************************************************)
(* Helpers for counterexample baits in MCLockFreeABA.tla.                                          *)
(***************************************************************************************************)

SomeReaderCommitted ==
    \E r \in Readers : rPhase[r] = "DONE" /\ rResult[r] = "committed"

SomeReaderObservedABA ==
    \E r \in Readers :
        /\ rPhase[r]   = "DONE"
        /\ rResult[r]  = "committed"
        /\ rSnapEpoch[r] # rPostEpoch[r]

====================================================================================================
