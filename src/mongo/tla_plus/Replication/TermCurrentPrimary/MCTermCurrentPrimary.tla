---- MODULE MCTermCurrentPrimary ----
\* MCTermCurrentPrimary defines bounds and state constraints for the
\* TermCurrentPrimary spec, so TLC can finish in seconds on a laptop.

EXTENDS TermCurrentPrimary

CONSTANT MaxTerm

StateConstraint ==
    /\ GlobalCurrentTerm <= MaxTerm

=============================================================================
