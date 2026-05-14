-------------------------------- MODULE MCSLSTopologyDiscovery -------------------------------------
\* This module defines SLSTopologyDiscovery.tla constants/constraints for model-checking.

EXTENDS SLSTopologyDiscovery

(**************************************************************************************************)
(* State Constraints.                                                                             *)
(**************************************************************************************************)

\* Bound the size of `pending' so the state space stays tractable: pending should always be a
\* subset of (Peers minus what is already known). This is naturally bounded but we add an explicit
\* constraint to prune obviously redundant intermediate states.
ConstraintPendingBounded ==
    \A d \in Discoverers : Cardinality(pending[d]) <= Cardinality(Peers)

\* Define symmetry for model checking to avoid exploring equivalent states.
Symmetry == Permutations(Discoverers) \union Permutations(Authorized) \union Permutations(Unauthorized)

(**************************************************************************************************)
(* Counterexamples (bait predicates).                                                             *)
(*                                                                                                *)
(* Set INVARIANT to one of these in the .cfg to confirm TLC can reach the corresponding state.   *)
(**************************************************************************************************)

\* A discoverer has converged to the full reachable set.
BaitFullyConverged == \A d \in Discoverers : Reachable \subseteq known[d]

\* An unauthorized rumor has been injected and dropped on the floor by AdmitPeer.
BaitRumorRejected == rumors # {}

\* A previously-known peer has been expired due to losing authorization.
BaitStaleExpired ==
    \E d \in Discoverers, p \in Peers :
        /\ p \in BootstrapSet
        /\ p \notin known[d]

====================================================================================================
