---- MODULE MCFCVInitDisaggLSN ----
\* Model-checking module for FCVInitDisaggLSN.tla. Adds the state-constraint
\* and symmetry instances expected by tlc2.TLC.

EXTENDS FCVInitDisaggLSN

\* Bound the search space. localStableTS is bounded by MaxLocalWrites, and
\* logServiceLSN is bounded by MaxLocalWrites because AdvanceLogServiceLSN
\* only fires when logServiceLSN < localStableTS - but we also keep a hard
\* StateConstraint on returnTrace cardinality to keep TLC's footprint flat.
StateConstraint ==
    /\ \A s \in Server :
        /\ localStableTS[s] <= MaxLocalWrites
        /\ logServiceLSN[s] <= MaxLocalWrites
    /\ Cardinality(returnTrace) <= 2 * Cardinality(Server)

\* Servers are interchangeable - any permutation of Server names is observed.
ServerSymmetry == Permutations(Server)
=============================================================================
