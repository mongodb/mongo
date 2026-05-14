---- MODULE MCFsyncLockDurableOpTimeBug ----
\* Model-checking harness for FsyncLockDurableOpTime.tla in the BUG model.
\*
\* This harness is identical to MCFsyncLockDurableOpTime in operator content;
\* a separate harness module is required because TLC binds one .cfg per .tla
\* module name. The .cfg pinned to this module flips BugEnabled to TRUE and
\* drops weak fairness on ReleaseGlobalS so the wedge-before-release window is
\* observable as a liveness counterexample.
\*
\* See FsyncLockDurableOpTime.tla for instructions.

EXTENDS FsyncLockDurableOpTime

StateConstraint ==
    /\ lastWritten <= MaxOpTime
    /\ durableOpTime <= MaxOpTime

\* Specification used by the bug cfg: drops WF on ReleaseGlobalS so TLC can
\* exhibit the stable-equilibrium wedge between AcquireGlobalS and
\* ReleaseGlobalS as a permanent counterexample, rather than waiting for the
\* lock to be released by fairness.
SpecBug ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(AdvanceDurable)

=============================================================================
