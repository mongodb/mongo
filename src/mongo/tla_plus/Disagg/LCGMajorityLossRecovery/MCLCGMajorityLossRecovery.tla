---- MODULE MCLCGMajorityLossRecovery ----
\* This module defines model-checking constants/constraints for
\* LCGMajorityLossRecovery.tla. See LCGMajorityLossRecovery.tla for full
\* documentation.

EXTENDS LCGMajorityLossRecovery

(**************************************************************************************************)
(* State Constraint. Bounds the model so TLC terminates.                                          *)
(**************************************************************************************************)

CONSTANTS MaxGeneration

StateConstraint ==
    /\ generation <= MaxGeneration
    /\ \A n \in Node : Len(log[n]) <= MaxLSN

(**************************************************************************************************)
(* Symmetry. Permuting node identifiers does not change behavior.                                 *)
(**************************************************************************************************)

NodeSymmetry == Permutations(Node)

================================================================================
