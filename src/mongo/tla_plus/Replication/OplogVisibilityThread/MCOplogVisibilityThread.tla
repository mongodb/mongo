---- MODULE MCOplogVisibilityThread ----
\* Model-checking harness for OplogVisibilityThread.tla. The .cfg file selects
\* which scenario to run (green or bug).

EXTENDS OplogVisibilityThread

\* Constrain the state space. opsCount is already bounded by MaxOps, but TLC
\* benefits from an explicit StateConstraint so the inductive search short-
\* circuits early.
StateConstraint == opsCount <= MaxOps

ReaderSymmetry == Permutations(Readers)
====================================================================
