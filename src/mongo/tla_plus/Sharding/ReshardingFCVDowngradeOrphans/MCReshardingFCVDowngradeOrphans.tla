---------------------------- MODULE MCReshardingFCVDowngradeOrphans ----------------------------
\* Model-check harness for ReshardingFCVDowngradeOrphans. Defines symmetry, a state-space
\* bound, and a small set of counterexample "bait" predicates that help confirm the model is
\* expressive enough to express each ticket's failure shape when the corresponding bug toggle
\* is flipped on.

EXTENDS ReshardingFCVDowngradeOrphans

\* Permute shards and namespaces to reduce equivalent states.
Symmetry == Permutations(Shards) \union Permutations(Namespaces)

\* State-space bound: cap on operations modelled.
StateConstraint == opsUsed <= MaxOps

(***************************************************************************************)
(* Bait predicates. With the green config, model-checking these properties yields a    *)
(* trace where the named scenario actually fires — confirming the model can express    *)
(* it. With a bug toggle on, the corresponding invariant violation is reachable.       *)
(***************************************************************************************)

\* A reshard donor that reaches the donating-initial-data phase actually happens.
BaitDonorReachesDonating ==
    \A n \in Namespaces : reshardDonor[n] # RS_DONATING_INITIAL_DATA

\* A migration commits and lands a pending range deletion.
BaitMigrationCommits ==
    \A s \in Shards : migState[s] # MIG_COMMITTED

\* setFCV aborts an in-flight reshard.
BaitFCVAbortsReshard ==
    fcv # FCV_LAST_LTS \/ \A n \in Namespaces : reshardDonor[n] # RS_ABORTED

\* A recipient that skipped index build commits — the SERVER-92437 mixed-FCV anomaly.
BaitMixedFCVCommit ==
    \A n \in Namespaces :
        ~(reshardRecipient[n] = RS_DONE /\ indexBuildSkipped[n] /\ fcv = FCV_LAST_LTS)

================================================================================================
