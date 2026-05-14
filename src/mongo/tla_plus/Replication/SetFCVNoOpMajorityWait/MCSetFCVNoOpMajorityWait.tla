---- MODULE MCSetFCVNoOpMajorityWait ----
\* MC harness for SetFCVNoOpMajorityWait.tla. Toggle AllowStaleOpTimeOnNoOp in
\* MCSetFCVNoOpMajorityWait.cfg to switch between the green (fixed) run and
\* the bug-reproducing run.

EXTENDS SetFCVNoOpMajorityWait

\* Symmetric bound on history length to keep state space tractable.
MCStateConstraint ==
    /\ StateConstraint
    /\ Len(history) <= MaxSetFCVOps
=========================================
