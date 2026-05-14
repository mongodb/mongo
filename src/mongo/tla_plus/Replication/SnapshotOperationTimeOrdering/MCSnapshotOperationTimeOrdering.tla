---- MODULE MCSnapshotOperationTimeOrdering ----
\* This module defines SnapshotOperationTimeOrdering.tla constants/constraints
\* for model-checking.
\* See SnapshotOperationTimeOrdering.tla for instructions.

EXTENDS SnapshotOperationTimeOrdering

\* State-space bound enforced through the cfg's CONSTRAINT clause.
StateConstraint == clusterTime <= MaxClusterTime

\* Allow TLC to collapse symmetric thread permutations. Threads are
\* interchangeable in this model (they all run the same read pipeline), so
\* permuting them does not produce semantically distinct behaviors.
ThreadSymmetry == Permutations(Thread)
=============================================================================
