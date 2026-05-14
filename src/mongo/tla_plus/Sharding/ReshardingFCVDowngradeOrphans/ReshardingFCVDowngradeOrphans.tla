\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE ReshardingFCVDowngradeOrphans -------------------------
\* Combined specification covering three related orphan/temp-collection issues that arise
\* when a setFeatureCompatibilityVersion (setFCV) downgrade interleaves with an in-flight
\* reshardCollection and the migration coordinator's range-deletion recovery:
\*
\*   SERVER-111230  reshardCollection abort during 'donating-initial-data' may leave
\*                  temp-collection metadata in the global catalog
\*                  (TempCollectionMetadataClearedOnFCVAbort).
\*   SERVER-92437   setFCV transitions to 'Downgrading to X.Y' BEFORE aborting in-flight
\*                  resharding, so a reshard can start under one feature flag and commit
\*                  under another (RangeDeletionsClearedOnReshardAbort: composed range
\*                  deletions left in pending:true on the recipient).
\*   SERVER-121914  FCV downgrade aborts an in-flight chunk migration after the migration
\*                  committed but before its range deletion is removed, leaving an orphan
\*                  range deletion in pending:true until lazy recovery
\*                  (NoStaleConfigChunksAfterFCVDowngrade).
\*
\* The three bugs share a common skeleton: a long-running multi-phase operation (a
\* reshard donor, a reshard recipient, or a moveChunk migration coordinator) writes
\* metadata across two collections (the global catalog plus a per-shard rangeDeletions
\* table); setFCV interrupts the operation between those writes; the recovery path is
\* gated on FCV state and silently elides the missing cleanup. Each bug has a separate
\* invariant + a separate boolean knob (BugTempCollLeak, BugRangeDelLeak, BugStaleChunks)
\* so that each invariant can be discharged independently against the green model and
\* re-falsified by flipping the corresponding bug knob.
\*
\* To run the model-checker, edit constants in MCReshardingFCVDowngradeOrphans.cfg, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Sharding/ReshardingFCVDowngradeOrphans

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Shards,             \* Set of shard ids participating in reshard + migration.
    Namespaces,         \* Set of collection namespaces.
    MaxOps,             \* Cap on the number of operations modelled (state-space bound).
    BugTempCollLeak,    \* SERVER-111230: skip temp-collection metadata cleanup on abort.
    BugRangeDelLeak,    \* SERVER-92437:  skip range-deletion cleanup on FCV abort.
    BugStaleChunks      \* SERVER-121914: skip config.chunks cleanup on FCV abort.

ASSUME Cardinality(Shards) >= 2
ASSUME Cardinality(Namespaces) >= 1
ASSUME MaxOps \in 1..10
ASSUME BugTempCollLeak \in BOOLEAN
ASSUME BugRangeDelLeak \in BOOLEAN
ASSUME BugStaleChunks \in BOOLEAN

(****************************************************************************)
(* FCV state machine.                                                       *)
(*                                                                          *)
(* setFCV walks a 4-state machine matching feature_compatibility_version.h: *)
(*   FCV_LATEST   -> FCV_DOWNGRADING -> FCV_LAST_LTS                        *)
(*                                  ^                                       *)
(*                                  |--- aborts in-flight reshard/migration *)
(*                                                                          *)
(* The buggy ordering modelled here: the FCV value is updated to            *)
(* FCV_DOWNGRADING BEFORE the abort logic fires (SERVER-92437 root cause).  *)
(****************************************************************************)
FCV_LATEST       == "fcvLatest"
FCV_DOWNGRADING  == "fcvDowngrading"
FCV_LAST_LTS     == "fcvLastLts"
FCV_UPGRADING    == "fcvUpgrading"
FCVStates == {FCV_LATEST, FCV_DOWNGRADING, FCV_LAST_LTS, FCV_UPGRADING}

(****************************************************************************)
(* Reshard donor/recipient state machine (resharding_donor_service.cpp +    *)
(* resharding_recipient_service.cpp). Only the states that participate in   *)
(* the bug pattern are modelled.                                            *)
(****************************************************************************)
RS_UNUSED                  == "unused"
RS_DONATING_INITIAL_DATA   == "donatingInitialData"  \* SERVER-111230 trigger
RS_DONATING_OPLOG_ENTRIES  == "donatingOplogEntries"
RS_CLONING                 == "cloning"              \* SERVER-92437 trigger
RS_BUILDING_INDEX          == "buildingIndex"
RS_APPLYING                == "applying"
RS_STRICT_CONSISTENCY      == "strictConsistency"
RS_DONE                    == "done"
RS_ABORTED                 == "aborted"
ReshardStates == {RS_UNUSED, RS_DONATING_INITIAL_DATA, RS_DONATING_OPLOG_ENTRIES,
                  RS_CLONING, RS_BUILDING_INDEX, RS_APPLYING, RS_STRICT_CONSISTENCY,
                  RS_DONE, RS_ABORTED}

(****************************************************************************)
(* Migration coordinator state machine (migration_util.cpp). Sufficient for *)
(* SERVER-121914: a migration that committed on the config server but whose *)
(* range-deletion entry has not yet been removed when setFCV aborts it.    *)
(****************************************************************************)
MIG_UNUSED      == "unused"
MIG_CLONING     == "cloning"
MIG_COMMITTED   == "committed"     \* SERVER-121914 trigger window opens here
MIG_CLEANED     == "cleaned"       \* range-deletion entry removed
MIG_ABORTED     == "aborted"
MigStates == {MIG_UNUSED, MIG_CLONING, MIG_COMMITTED, MIG_CLEANED, MIG_ABORTED}

(****************************************************************************)
(* Range-deletion entry lifecycle (rangeDeletions collection).              *)
(*  - "pending"   = pending:true, waits on migration coordinator signal     *)
(*  - "ready"     = pending:false, range-deleter eligible to run            *)
(*  - "absent"    = entry not present                                       *)
(****************************************************************************)
RD_ABSENT  == "absent"
RD_PENDING == "pending"
RD_READY   == "ready"
RDStates == {RD_ABSENT, RD_PENDING, RD_READY}

(* Global variables *)
VARIABLE fcv                 \* Current FCV state.
VARIABLE reshardDonor        \* [ns -> ReshardStates] donor-side reshard state.
VARIABLE reshardRecipient    \* [ns -> ReshardStates] recipient-side reshard state.
VARIABLE migState            \* [shard -> MigStates] migration coordinator state.
VARIABLE rangeDel            \* [shard -> RDStates] rangeDeletions entry on this shard.
VARIABLE tempCollMeta        \* [ns -> BOOLEAN] temp-collection metadata present in catalog.
VARIABLE configChunks        \* [ns -> BOOLEAN] stale config.chunks entries pinned.
VARIABLE indexBuildSkipped   \* [ns -> BOOLEAN] recipient skipped buildingIndex phase.
VARIABLE mixedFcvCommit      \* [ns -> BOOLEAN] ghost: recipient went done under
                             \* downgrading FCV (the SERVER-92437 anomaly fingerprint).
VARIABLE opsUsed             \* Total operations modelled (state-space bound).

vars == <<fcv, reshardDonor, reshardRecipient, migState, rangeDel, tempCollMeta,
          configChunks, indexBuildSkipped, mixedFcvCommit, opsUsed>>

(****************************************************************************)
(* Type invariant.                                                          *)
(****************************************************************************)
TypeOK ==
    /\ fcv \in FCVStates
    /\ reshardDonor \in [Namespaces -> ReshardStates]
    /\ reshardRecipient \in [Namespaces -> ReshardStates]
    /\ migState \in [Shards -> MigStates]
    /\ rangeDel \in [Shards -> RDStates]
    /\ tempCollMeta \in [Namespaces -> BOOLEAN]
    /\ configChunks \in [Namespaces -> BOOLEAN]
    /\ indexBuildSkipped \in [Namespaces -> BOOLEAN]
    /\ mixedFcvCommit \in [Namespaces -> BOOLEAN]
    /\ opsUsed \in 0..MaxOps

(****************************************************************************)
(* Initial state: pristine cluster, FCV at latest, all state machines idle. *)
(****************************************************************************)
Init ==
    /\ fcv = FCV_LATEST
    /\ reshardDonor = [n \in Namespaces |-> RS_UNUSED]
    /\ reshardRecipient = [n \in Namespaces |-> RS_UNUSED]
    /\ migState = [s \in Shards |-> MIG_UNUSED]
    /\ rangeDel = [s \in Shards |-> RD_ABSENT]
    /\ tempCollMeta = [n \in Namespaces |-> FALSE]
    /\ configChunks = [n \in Namespaces |-> FALSE]
    /\ indexBuildSkipped = [n \in Namespaces |-> FALSE]
    /\ mixedFcvCommit = [n \in Namespaces |-> FALSE]
    /\ opsUsed = 0

TickOp == opsUsed' = opsUsed + 1
WithinBudget == opsUsed < MaxOps

(****************************************************************************)
(* Reshard donor transitions. The donor enters donating-initial-data and    *)
(* writes temp-collection metadata to the global catalog. Aborting in this  *)
(* phase requires the cleanup hook to wipe that metadata (SERVER-111230).   *)
(****************************************************************************)
ReshardStartDonor(n) ==
    /\ WithinBudget
    /\ reshardDonor[n] = RS_UNUSED
    /\ reshardRecipient[n] = RS_UNUSED
    /\ reshardDonor' = [reshardDonor EXCEPT ![n] = RS_DONATING_INITIAL_DATA]
    /\ reshardRecipient' = [reshardRecipient EXCEPT ![n] = RS_CLONING]
    /\ tempCollMeta' = [tempCollMeta EXCEPT ![n] = TRUE]
    /\ configChunks' = [configChunks EXCEPT ![n] = TRUE]
    \* SERVER-92437: recipient skips index build only if FCV currently supports flag.
    /\ indexBuildSkipped' = [indexBuildSkipped EXCEPT ![n] = (fcv = FCV_LATEST)]
    /\ TickOp
    /\ UNCHANGED <<fcv, migState, rangeDel, mixedFcvCommit>>

ReshardAdvanceDonor(n) ==
    /\ reshardDonor[n] = RS_DONATING_INITIAL_DATA
    /\ reshardDonor' = [reshardDonor EXCEPT ![n] = RS_DONATING_OPLOG_ENTRIES]
    /\ UNCHANGED <<fcv, reshardRecipient, migState, rangeDel, tempCollMeta,
                   configChunks, indexBuildSkipped, mixedFcvCommit, opsUsed>>

ReshardAdvanceRecipient(n) ==
    /\ reshardRecipient[n] = RS_CLONING
    /\ reshardRecipient' = [reshardRecipient EXCEPT
            ![n] = IF indexBuildSkipped[n] THEN RS_APPLYING ELSE RS_BUILDING_INDEX]
    /\ UNCHANGED <<fcv, reshardDonor, migState, rangeDel, tempCollMeta,
                   configChunks, indexBuildSkipped, mixedFcvCommit, opsUsed>>

\* The fix corresponding to SERVER-92437: a reshard recipient that decided to skip
\* the buildingIndex phase under FCV_LATEST must NOT commit while FCV is in the
\* downgrading state. The setFCV abort path is responsible for aborting it
\* (FCVAbortInFlight), not letting it slip through to RS_DONE. The BugRangeDelLeak
\* toggle relaxes this gate to reproduce the original anomaly.
ReshardCommit(n) ==
    /\ reshardDonor[n] = RS_DONATING_OPLOG_ENTRIES
    /\ reshardRecipient[n] \in {RS_APPLYING, RS_BUILDING_INDEX}
    /\ \/ fcv = FCV_LATEST
       \/ ~indexBuildSkipped[n]
       \/ BugRangeDelLeak  \* Bug ON: commit allowed even mid-downgrade.
    /\ reshardDonor' = [reshardDonor EXCEPT ![n] = RS_DONE]
    /\ reshardRecipient' = [reshardRecipient EXCEPT ![n] = RS_DONE]
    /\ tempCollMeta' = [tempCollMeta EXCEPT ![n] = FALSE]
    \* The temp collection's chunks become the production chunks (rename), so the
    \* stale-config-chunks bit clears only because rename succeeded.
    /\ configChunks' = [configChunks EXCEPT ![n] = FALSE]
    \* Stamp the ghost variable: did this commit happen mid-downgrade with a
    \* skipped index build? That fingerprint is the SERVER-92437 anomaly.
    /\ mixedFcvCommit' = [mixedFcvCommit EXCEPT
            ![n] = mixedFcvCommit[n]
                    \/ (fcv = FCV_DOWNGRADING /\ indexBuildSkipped[n])]
    /\ UNCHANGED <<fcv, migState, rangeDel, indexBuildSkipped, opsUsed>>

(****************************************************************************)
(* Migration coordinator transitions. The committed-but-not-cleaned window  *)
(* is the SERVER-121914 trigger.                                            *)
(****************************************************************************)
MigStart(s) ==
    /\ WithinBudget
    /\ migState[s] = MIG_UNUSED
    /\ migState' = [migState EXCEPT ![s] = MIG_CLONING]
    /\ rangeDel' = [rangeDel EXCEPT ![s] = RD_PENDING]
    /\ TickOp
    /\ UNCHANGED <<fcv, reshardDonor, reshardRecipient, tempCollMeta, configChunks,
                   indexBuildSkipped, mixedFcvCommit>>

MigCommit(s) ==
    /\ migState[s] = MIG_CLONING
    /\ migState' = [migState EXCEPT ![s] = MIG_COMMITTED]
    /\ UNCHANGED <<fcv, reshardDonor, reshardRecipient, rangeDel, tempCollMeta,
                   configChunks, indexBuildSkipped, mixedFcvCommit, opsUsed>>

MigFinishCleanup(s) ==
    /\ migState[s] = MIG_COMMITTED
    /\ migState' = [migState EXCEPT ![s] = MIG_CLEANED]
    /\ rangeDel' = [rangeDel EXCEPT ![s] = RD_ABSENT]
    /\ UNCHANGED <<fcv, reshardDonor, reshardRecipient, tempCollMeta, configChunks,
                   indexBuildSkipped, mixedFcvCommit, opsUsed>>

(****************************************************************************)
(* setFCV transitions.                                                      *)
(*                                                                          *)
(* The real-world sequence (and the SERVER-92437 root cause) is:            *)
(*   1. Write FCV = "downgrading"                                           *)
(*   2. THEN issue abort for in-flight reshard/migration                    *)
(*                                                                          *)
(* The window between (1) and (2) is what allows a reshard's recipient to   *)
(* skip index creation under the old FCV and then commit under the new one. *)
(****************************************************************************)
FCVBeginDowngrade ==
    /\ WithinBudget
    /\ fcv = FCV_LATEST
    /\ fcv' = FCV_DOWNGRADING
    /\ TickOp
    /\ UNCHANGED <<reshardDonor, reshardRecipient, migState, rangeDel, tempCollMeta,
                   configChunks, indexBuildSkipped, mixedFcvCommit>>

\* setFCV's abort phase. Aborts all in-flight reshard and migration ops. Cleanup
\* of derived state (temp-collection metadata, range deletions, config.chunks) is
\* conditional on the bug toggles to model each ticket's defect.
FCVAbortInFlight ==
    /\ fcv = FCV_DOWNGRADING
    /\ \E ns \in Namespaces :
        reshardDonor[ns] \in {RS_DONATING_INITIAL_DATA, RS_DONATING_OPLOG_ENTRIES,
                              RS_CLONING}
        \/ reshardRecipient[ns] \in {RS_CLONING, RS_BUILDING_INDEX, RS_APPLYING}
        \/ (\E sh \in Shards : migState[sh] = MIG_COMMITTED)
    /\ reshardDonor' = [n \in Namespaces |->
            IF reshardDonor[n] \in {RS_DONATING_INITIAL_DATA, RS_DONATING_OPLOG_ENTRIES}
            THEN RS_ABORTED ELSE reshardDonor[n]]
    /\ reshardRecipient' = [n \in Namespaces |->
            IF reshardRecipient[n] \in {RS_CLONING, RS_BUILDING_INDEX, RS_APPLYING}
            THEN RS_ABORTED ELSE reshardRecipient[n]]
    /\ migState' = [sh \in Shards |->
            IF migState[sh] \in {MIG_CLONING, MIG_COMMITTED}
            THEN MIG_ABORTED ELSE migState[sh]]
    \* SERVER-111230 bug toggle: temp collection metadata not cleared on abort.
    /\ tempCollMeta' = [n \in Namespaces |->
            IF reshardDonor[n] # RS_UNUSED /\ ~BugTempCollLeak
            THEN FALSE ELSE tempCollMeta[n]]
    \* SERVER-121914 bug toggle: config.chunks for the temp-namespace not cleared.
    /\ configChunks' = [n \in Namespaces |->
            IF reshardDonor[n] # RS_UNUSED /\ ~BugStaleChunks
            THEN FALSE ELSE configChunks[n]]
    \* SERVER-92437 bug toggle: range deletions left in pending:true on abort.
    \* The setFCV abort path must clear pending range-deletion entries on ALL
    \* aborted migrations, regardless of whether they had committed or were still
    \* cloning when the abort fired.
    /\ rangeDel' = [sh \in Shards |->
            IF migState[sh] \in {MIG_CLONING, MIG_COMMITTED} /\ ~BugRangeDelLeak
            THEN RD_ABSENT ELSE rangeDel[sh]]
    /\ UNCHANGED <<fcv, indexBuildSkipped, mixedFcvCommit, opsUsed>>

FCVFinishDowngrade ==
    /\ fcv = FCV_DOWNGRADING
    /\ \A n \in Namespaces :
        /\ reshardDonor[n] \in {RS_UNUSED, RS_ABORTED, RS_DONE}
        /\ reshardRecipient[n] \in {RS_UNUSED, RS_ABORTED, RS_DONE}
    /\ \A sh \in Shards : migState[sh] \in {MIG_UNUSED, MIG_ABORTED, MIG_CLEANED}
    /\ fcv' = FCV_LAST_LTS
    /\ UNCHANGED <<reshardDonor, reshardRecipient, migState, rangeDel, tempCollMeta,
                   configChunks, indexBuildSkipped, mixedFcvCommit, opsUsed>>

(****************************************************************************)
(* Lazy migration recovery. SERVER-121914 notes that lazy recovery DOES     *)
(* eventually clean pending:true range deletions, but it is gated on a      *)
(* subsequent query/stepup. We model it as an always-enabled action so the  *)
(* invariant is "after FCV downgrade settles, lazy recovery is the only way *)
(* to clear stragglers" — which the invariant rules out in the green model. *)
(****************************************************************************)
LazyMigrationRecovery(s) ==
    /\ migState[s] = MIG_ABORTED
    /\ rangeDel[s] = RD_PENDING
    /\ rangeDel' = [rangeDel EXCEPT ![s] = RD_ABSENT]
    /\ UNCHANGED <<fcv, reshardDonor, reshardRecipient, migState, tempCollMeta,
                   configChunks, indexBuildSkipped, mixedFcvCommit, opsUsed>>

(****************************************************************************)
(* Terminal stutter. Once FCV has settled at LAST_LTS and there are no       *)
(* pending range-deletion or in-flight reshard actions, the system is in a   *)
(* steady state; allow an explicit no-op so TLC's deadlock check doesn't    *)
(* flag the legitimate end.                                                 *)
(****************************************************************************)
TerminalStutter ==
    /\ fcv = FCV_LAST_LTS
    /\ \A n \in Namespaces :
            reshardDonor[n] \in {RS_UNUSED, RS_ABORTED, RS_DONE}
            /\ reshardRecipient[n] \in {RS_UNUSED, RS_ABORTED, RS_DONE}
    /\ \A s \in Shards :
            migState[s] \in {MIG_UNUSED, MIG_ABORTED, MIG_CLEANED}
            /\ rangeDel[s] \in {RD_ABSENT}
    /\ UNCHANGED vars

(****************************************************************************)
(* Next-state relation.                                                     *)
(****************************************************************************)
Next ==
    \/ \E n \in Namespaces : ReshardStartDonor(n)
    \/ \E n \in Namespaces : ReshardAdvanceDonor(n)
    \/ \E n \in Namespaces : ReshardAdvanceRecipient(n)
    \/ \E n \in Namespaces : ReshardCommit(n)
    \/ \E s \in Shards : MigStart(s)
    \/ \E s \in Shards : MigCommit(s)
    \/ \E s \in Shards : MigFinishCleanup(s)
    \/ \E s \in Shards : LazyMigrationRecovery(s)
    \/ FCVBeginDowngrade
    \/ FCVAbortInFlight
    \/ FCVFinishDowngrade
    \/ TerminalStutter

Spec == Init /\ [][Next]_vars

(****************************************************************************)
(* Safety invariants. One per ticket; each can be falsified independently   *)
(* by flipping the corresponding bug toggle.                                *)
(****************************************************************************)

\* SERVER-111230. After setFCV downgrade settles (FCV reaches LAST_LTS) and there
\* is no in-flight reshard, no temp-collection metadata may remain in the catalog.
TempCollectionMetadataClearedOnFCVAbort ==
    (fcv = FCV_LAST_LTS) =>
        \A n \in Namespaces :
            (reshardDonor[n] \in {RS_UNUSED, RS_ABORTED, RS_DONE}
             /\ reshardRecipient[n] \in {RS_UNUSED, RS_ABORTED, RS_DONE})
            => tempCollMeta[n] = FALSE

\* SERVER-92437. After FCV downgrade settles, no shard may carry a pending:true
\* range-deletion entry for an aborted migration: the setFCV abort path is
\* required to clear it synchronously, NOT defer to lazy recovery.
RangeDeletionsClearedOnReshardAbort ==
    (fcv = FCV_LAST_LTS) =>
        \A s \in Shards :
            (migState[s] = MIG_ABORTED) => (rangeDel[s] = RD_ABSENT)

\* SERVER-121914. After FCV downgrade settles, no namespace may have stale
\* config.chunks entries for a temp-namespace whose owning reshard was aborted.
NoStaleConfigChunksAfterFCVDowngrade ==
    (fcv = FCV_LAST_LTS) =>
        \A n \in Namespaces :
            (reshardDonor[n] = RS_ABORTED) => configChunks[n] = FALSE

\* Composite consistency: no reshard recipient may transition into RS_DONE while
\* FCV is in the downgrading state with indexBuildSkipped=TRUE. The ghost
\* `mixedFcvCommit` records that fingerprint at commit-time; if the bug toggle
\* BugRangeDelLeak is OFF, no namespace should ever stamp it.
IndexBuildDecisionFCVConsistent ==
    \A n \in Namespaces : mixedFcvCommit[n] = FALSE

(****************************************************************************)
(* Liveness. With the bugs disabled, the system always makes progress to a  *)
(* steady FCV_LAST_LTS state without stranded metadata.                     *)
(****************************************************************************)
EventuallyConsistent ==
    <>[](fcv = FCV_LAST_LTS =>
            /\ \A n \in Namespaces : tempCollMeta[n] = FALSE
            /\ \A s \in Shards : rangeDel[s] = RD_ABSENT
            /\ \A n \in Namespaces : configChunks[n] = FALSE)

========================================================================================
