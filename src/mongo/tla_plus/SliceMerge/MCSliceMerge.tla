---- MODULE MCSliceMerge ----
\* This module defines MCSliceMerge.tla constants/constraints for model-checking.

EXTENDS SliceMerge

CONSTANT MaxRequests

(**************************************************************************************************)
(* State Constraint. Used for model checking only.                                                *)
(**************************************************************************************************)

StateConstraint ==
    MaxRequests > totalRequests

=============================================================================
