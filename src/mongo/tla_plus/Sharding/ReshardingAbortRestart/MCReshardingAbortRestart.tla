---------------------------- MODULE MCReshardingAbortRestart ---------------------------------------
\* This module defines ReshardingAbortRestart.tla constants/constraints for model-checking. The
\* corresponding .cfg points either at the green configuration (the fix proposed in SERVER-125589
\* is applied; `AllowResumeFromAbortedWithoutPrepare = FALSE') or at the bug configuration
\* (pre-fix; flag = TRUE).
\*
\* To run the bug configuration, swap CONSTANT line in MCReshardingAbortRestart.cfg to set
\* `AllowResumeFromAbortedWithoutPrepare = TRUE'. TLC will produce a counterexample to
\* `NoCommitFromAbortedWithoutPrepare' within a handful of states.

EXTENDS ReshardingAbortRestart

(**************************************************************************************************)
(* State Constraints                                                                              *)
(**************************************************************************************************)

\* Cap the number of "in-flight" writes the model considers. Two writes are enough to expose the
\* silent-loss counterexample (one pre-abort, one post-restart) and keep TLC fast.
ConstraintBoundedSubmitted == Cardinality(submitted) <= 2

\* Bound the auxiliary refusal counter so TLC's state space stays finite even when sub-routers
\* keep retrying. Refusals are observability only - the substance of the model is in `state' and
\* `stagedForCommit'.
ConstraintBoundedRejects == subRouterReject <= 4

(**************************************************************************************************)
(* Counterexamples / bait                                                                         *)
(**************************************************************************************************)

\* The recovery path was actually executed (Block 1 -> Block 2). Useful for confirming the model
\* exercises the recovery-path branch and is not just exploring client-driven aborts.
BaitRecoveryExecuted == recovery # recDone

\* The participant was revived at least once. Always FALSE in the green model; TRUE in the bug
\* model just before the silent-loss state is reached.
BaitRevivalHappened == revivals = 0

====================================================================================================
