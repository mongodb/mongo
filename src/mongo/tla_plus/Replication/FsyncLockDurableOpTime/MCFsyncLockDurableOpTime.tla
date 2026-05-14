---- MODULE MCFsyncLockDurableOpTime ----
\* Model-checking harness for FsyncLockDurableOpTime.tla.
\* See FsyncLockDurableOpTime.tla for instructions.

EXTENDS FsyncLockDurableOpTime

\* State-space bound. lastWritten and durableOpTime both live in 0..MaxOpTime,
\* so this is enforced statically by the variable type already; we keep the
\* explicit constraint for clarity and to make tightening the bound during
\* exploratory runs a one-line change.
StateConstraint ==
    /\ lastWritten <= MaxOpTime
    /\ durableOpTime <= MaxOpTime

=============================================================================
