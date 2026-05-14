\* Copyright 2026 MongoDB, Inc.

---- MODULE MCRollbackRetryableApplyOps ----
EXTENDS RollbackRetryableApplyOps, TLC

\* Symmetry over logical session ids to shrink the state space.
LsidSymmetry == Permutations(Lsids)

\* Bound state-space exploration. Without this TLC will explore arbitrary
\* interleavings of appends + rollback phases.
StateConstraint ==
    /\ Len(oplog) <= MaxLogLen
    /\ rollbackCount <= MaxRollbacks

=============================================================================
