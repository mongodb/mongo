---------------------------- MODULE MCChangeStreamTopologyChange ----------------------------
\* This module defines ChangeStreamTopologyChange.tla constants/constraints for model-checking.
\*
\* The .cfg toggles the AllowNewShardCursorBelowResumeToken constant. With FALSE the spec
\* holds; with TRUE TLC finds a counterexample to NoEventBeforeResumeToken that matches the
\* SERVER-48386 / SERVER-124540 race shape.

EXTENDS ChangeStreamTopologyChange

(**************************************************************************************************)
(* State constraints                                                                              *)
(**************************************************************************************************)

\* Cap the explored state space. Without these caps a full sweep is unbounded; the bounds chosen
\* below are sufficient to surface the counterexample (one addShard + one pre-future write).
StateConstraint ==
    /\ coordinatorTime <= MaxClusterTime
    /\ writes <= MaxWrites
    /\ topologyChanges <= MaxTopologyChanges
    /\ Len(consumerDelivered) <= MaxWrites + 2

\* Symmetry across shard ids: any permutation of ShardIds is an equivalent model state, so
\* TLC can prune.
Symmetry == Permutations(ShardIds)

(**************************************************************************************************)
(* Counterexample baits (for sanity-checking via INVARIANT, comment in/out as desired).           *)
(**************************************************************************************************)

\* The consumer has been delivered at least one event. Used to confirm the model can produce
\* delivery activity at all under tightened constants.
BaitConsumerDeliveredOne == Len(consumerDelivered) = 0

\* A new shard has been added (i.e., at least one shard joined after Init).
BaitNewShardAdded ==
    \A s \in ShardIds : (s \notin InitialActiveShards) => shardAddedAt[s] = UninitializedTs

\* A stepdown occurred on some shard.
BaitStepdownOccurred == \A s \in ShardIds : shardPrimaryEpoch[s] = 1

====================================================================================================
