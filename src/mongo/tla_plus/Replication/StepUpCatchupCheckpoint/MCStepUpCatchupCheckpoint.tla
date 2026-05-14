---- MODULE MCStepUpCatchupCheckpoint ----
\* Model-checking harness for StepUpCatchupCheckpoint.tla. The bound on terms
\* and oplog length keeps the state space finite; see StateConstraint.

EXTENDS StepUpCatchupCheckpoint

StateConstraint ==
    /\ GlobalTerm <= MaxTerm
    /\ \A s \in Server : Len(oplog[s]) <= MaxLogLen

\* Symmetry over server identities collapses isomorphic states. Safe for the
\* safety-only model-check we run here; do not enable when checking liveness.
ServerSymmetry == Permutations(Server)
==========================================
