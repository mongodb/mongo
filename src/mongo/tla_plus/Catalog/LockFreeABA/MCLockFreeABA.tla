---------------------------------- MODULE MCLockFreeABA ------------------------------------------
\* Constants and constraints for model-checking LockFreeABA.tla.

EXTENDS LockFreeABA

(***************************************************************************************************)
(* State constraints.                                                                              *)
(*                                                                                                 *)
(* The state space is naturally bounded by MAX_EPOCH: every placement-changing DDL bumps the       *)
(* epoch and once nextEpoch exceeds MAX_EPOCH no further DDLs fire. We additionally stop          *)
(* exploring once every reader is DONE.                                                            *)
(***************************************************************************************************)

StateConstraint ==
    /\ catalogEpoch <= MAX_EPOCH
    /\ \E r \in Readers : rPhase[r] # "DONE"

\* Symmetry on Readers and Writers cuts the state space significantly: the hazard does not care
\* which specific reader/writer thread fires which step.
Symmetry == Permutations(Readers) \union Permutations(Writers)

(***************************************************************************************************)
(* Counterexample baits.                                                                           *)
(*                                                                                                 *)
(* Negate them to force TLC to produce an example trace.                                           *)
(***************************************************************************************************)

BaitNoCommit       == ~SomeReaderCommitted
BaitNoABA          == ~SomeReaderObservedABA
BaitTrace          == TLCGet("level") < 60

====================================================================================================
