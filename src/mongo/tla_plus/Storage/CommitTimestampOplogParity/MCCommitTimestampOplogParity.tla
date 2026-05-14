---- MODULE MCCommitTimestampOplogParity ----
\* This module defines CommitTimestampOplogParity.tla constants/constraints for
\* model-checking. See CommitTimestampOplogParity.tla for instructions.

EXTENDS CommitTimestampOplogParity

\* All writes are interchangeable; use TLC symmetry to shrink the state space.
WriteSymmetry == Permutations(Writes)

\* StateConstraint is enforced by the bounded MaxTimestamp; declare a trivial
\* one so the .cfg has a single named constraint to reference uniformly with
\* the rest of the tla_plus suite.
StateConstraint == nextOplogTs <= MaxTimestamp + 1

=============================================================================
