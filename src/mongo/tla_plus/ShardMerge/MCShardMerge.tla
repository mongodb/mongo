---- MODULE MCShardMerge ----
\* This module defines MCShardMerge.tla constants/constraints for model-checking.

EXTENDS ShardMerge

CONSTANT MaxRequests

(**************************************************************************************************)
(* State Constraint. Used for model checking only.                                                *)
(**************************************************************************************************)

StateConstraint ==
    MaxRequests > totalRequests

=============================================================================
