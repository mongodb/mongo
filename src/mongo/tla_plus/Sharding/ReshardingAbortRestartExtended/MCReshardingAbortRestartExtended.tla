--------------------------- MODULE MCReshardingAbortRestartExtended --------------------------------
\* Model-check harness for ReshardingAbortRestartExtended.tla. Two cfg variants:
\*   - MCReshardingAbortRestartExtended.cfg      (green: AllowResumeFromPrepared = FALSE)
\*   - MCReshardingAbortRestartExtended_bug.cfg  (bug:   AllowResumeFromPrepared = TRUE)
\*
\* Both cfgs leave AllowFirstStatementRetry = TRUE - the spec must prove that the safe
\* SERVER-46679 retry path stays safe even when the prepared-resume bug is enabled.

EXTENDS ReshardingAbortRestartExtended

(**************************************************************************************************)
(* State Constraints                                                                              *)
(**************************************************************************************************)

\* Two writes are enough to expose the silent-loss counterexample on either the wave-2 branch or
\* the new kPrepared branch (one pre-abort / pre-prepare, one post-restart).
ConstraintBoundedSubmitted == Cardinality(submitted) <= 2

\* Bound the auxiliary refusal counter so TLC stays finite even when sub-routers keep retrying.
ConstraintBoundedRejects == subRouterReject <= 4

(**************************************************************************************************)
(* Counterexamples / bait                                                                         *)
(**************************************************************************************************)

\* The recovery path was actually executed against a prepared participant.
BaitPreparedRecoveryExecuted == state # kPrepared \/ recovery # recInGap

\* A first-statement retry actually happened. Asserting FALSE confirms the safe path is exercised.
BaitFirstStatementRetried == txnRetryCounter = 0

\* The participant was revived from kPrepared at least once. Always FALSE in the green cfg.
BaitRevivalFromPrepared == revivals = 0

====================================================================================================
