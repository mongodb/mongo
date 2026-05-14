---- MODULE MCStepdownPDIBDeadlock ----
\* Model-checking harness for StepdownPDIBDeadlock.tla.
\*
\* By default this harness model-checks the GREEN (patched) configuration:
\* AllowPDIBHoldDuringStepdown = FALSE.  See MCStepdownPDIBDeadlock.cfg
\* for that run.
\*
\* The accompanying file MCStepdownPDIBDeadlock_bug.cfg flips the bug
\* toggle so TLC can produce a counterexample trace of the original
\* SERVER-126266 deadlock.

EXTENDS StepdownPDIBDeadlock

\* No additional state-space bound beyond the bounded variables defined
\* in the base spec.  The model is finite by construction:
\*   - rstlQueue is bounded by |Threads| = 3 because no thread enqueues
\*     itself twice (enforced in BgSyncEnqueue and PDIBStart guards).
\*   - commitQuorumVotes is a subset of 1..MaxQuorum.
\*   - All other variables range over finite domains.
\*
\* The "natural" symmetry of secondaries justifies a SYMMETRY clause in
\* the cfg.  Provide it here as an operator over the secondary id set.
QuorumSymmetry == Permutations(1..MaxQuorum)

================================================================================
