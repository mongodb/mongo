---- MODULE MCRaftMongoReplTimestamp ----
\* This module defines RaftMongoReplTimestamp.tla constants/constraints for model-checking.
\* See RaftMongoReplTimestamp.tla for instructions.

EXTENDS RaftMongoReplTimestamp

CONSTANT MaxTerm
CONSTANT MaxLogLen
CONSTANT MaxRestartTimes
CONSTANT MaxFailoverTimes

StateConstraint ==
    /\ GlobalCurrentTerm <= MaxTerm
    /\ \forall i \in Server: /\ Len(log[i]) <= MaxLogLen
                             /\ restartTimes[i] <= MaxRestartTimes
                             /\ failoverTimes[i] <= MaxFailoverTimes

ServerSymmetry == Permutations(Server)
=============================================================================
