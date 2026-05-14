---------------------------- MODULE MCShardRegistryHostRotation ------------------------------------
\* This module defines ShardRegistryHostRotation.tla constants/constraints for model-checking.

EXTENDS ShardRegistryHostRotation

(**************************************************************************************************)
(* State constraints.                                                                              *)
(**************************************************************************************************)

\* Bound the state space along the version axes. The interesting dynamics are exhausted within a
\* handful of rotations and topology bumps; allowing arbitrary growth blows up the search without
\* surfacing new behaviours.
StateConstraint ==
    /\ cfgConfigVersion <= MAX_ROTATIONS + 1
    /\ cfgTopologyTime <= MAX_TOPOLOGY_BUMPS + 1
    /\ srCachedConfigVersion <= MAX_ROTATIONS + 1
    /\ srLastSeenTopologyTime <= MAX_TOPOLOGY_BUMPS + 1

(**************************************************************************************************)
(* Symmetry.                                                                                       *)
(**************************************************************************************************)

\* Hosts are interchangeable up to identity; permutations of the host universe preserve all
\* observable behaviour.
Symmetry == Permutations(Hosts)

====================================================================================================
