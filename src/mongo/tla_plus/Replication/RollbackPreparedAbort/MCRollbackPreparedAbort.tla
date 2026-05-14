---- MODULE MCRollbackPreparedAbort ----
\* This module defines MCRollbackPreparedAbort.tla constants/constraints for
\* model-checking the SERVER-125802 spec.

EXTENDS RollbackPreparedAbort

CONSTANTS MaxCheckpoints

StateConstraint == \A s \in Server :
                       Cardinality(checkpoints[s]) <= MaxCheckpoints

ServerSymmetry == Permutations(Server)
=============================================================================
