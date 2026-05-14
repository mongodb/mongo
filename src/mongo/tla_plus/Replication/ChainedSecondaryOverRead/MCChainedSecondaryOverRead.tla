---- MODULE MCChainedSecondaryOverRead ----
\* Model-checking harness for ChainedSecondaryOverRead.tla.
\* See ChainedSecondaryOverRead.tla for instructions.

EXTENDS ChainedSecondaryOverRead

CONSTANT MaxTerm
CONSTANT MaxLogLen

StateConstraint ==
    /\ GlobalCurrentTerm <= MaxTerm
    /\ \A i \in Server : Len(log[i]) <= MaxLogLen

ServerSymmetry == Permutations(Server)
=============================================================================
