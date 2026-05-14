---------------------------------- MODULE MCUnshardChunklessCSS ------------------------------------
\* This module defines UnshardChunklessCSS.tla constants/constraints for model-checking.

EXTENDS UnshardChunklessCSS

(**************************************************************************************************)
(* State constraints. Bound state-space exploration.                                              *)
(**************************************************************************************************)

StateConstraint ==
    /\ unshardsRemaining >= 0
    \* Once we've exhausted the unshard budget and seen at least one drop scan, allow stuttering.
    /\ nextUUID < 1000

\* Symmetry over shards and namespaces. We do NOT include UUIDs in the symmetry set: they are Nat
\* values minted in order and distinguishing them keeps counterexample traces readable.
Symmetry == Permutations(Shards) \union Permutations(NameSpaces)

====================================================================================================
