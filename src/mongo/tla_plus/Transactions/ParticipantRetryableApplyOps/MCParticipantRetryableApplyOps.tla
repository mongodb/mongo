---------------------------- MODULE MCParticipantRetryableApplyOps ----------------------------
\* This module defines ParticipantRetryableApplyOps.tla constants/constraints for model-checking.

EXTENDS ParticipantRetryableApplyOps

(***************************************************************************)
(* State Constraints.                                                      *)
(***************************************************************************)

StateConstraint ==
    \* Stop exploring once the oplog cap is reached. The invariants are about the set
    \* recognized from the durable chain, not about further interleavings beyond the cap.
    /\ Len(oplog) < MAX_OPLOG_ENTRIES

\* Symmetry over stmtIds: TLC need not enumerate equivalent orderings.
Symmetry == Permutations(StmtIds)

(***************************************************************************)
(* Bait invariants — selectively enable one to produce a counter-example.  *)
(***************************************************************************)

\* Produces a trace exhibiting at least one atomic-applyOps entry.
BaitAtLeastOneAtomicApplyOps ==
    ~ \E i \in DOMAIN oplog : oplog[i].kind = ATOMIC_APPLYOPS

\* Produces a trace where the LEGACY recognizer diverges from the durable set —
\* i.e. the very bug SERVER-126375 fixes. Should produce a counter-example as soon as
\* any atomic-applyOps entry is written.
BaitLegacyDivergence ==
    LegacyRecognizedStmtIds = DurableStmtIds

============================================================================
