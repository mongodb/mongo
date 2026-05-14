---- MODULE MCDefaultReadConcernAfterClusterTime ----
\* Model-checking module for DefaultReadConcernAfterClusterTime.tla.
\* See DefaultReadConcernAfterClusterTime.tla for instructions.

EXTENDS DefaultReadConcernAfterClusterTime

\* Bound the state space: at most MaxReads issued reads.
CONSTANT MaxReads

StateConstraint ==
    /\ Len(oplog) <= MaxOpTime
    /\ Cardinality(reads) <= MaxReads
=============================================================================
