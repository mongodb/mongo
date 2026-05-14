------------------- MODULE MCRetryableInternalTxnRYOW -------------------
\* Model-checking module for RetryableInternalTxnRYOW. The default
\* configuration file used by model-check.sh is
\* MCRetryableInternalTxnRYOW.cfg, which mirrors MC.cfg (safe path,
\* SHORT_CIRCUIT_ON_PARTIAL_TXN = FALSE) and asserts the
\* ReadYourOwnWrites invariant.
\*
\* To reproduce the SERVER-99784 bug, run with MC_bug.cfg
\* (SHORT_CIRCUIT_ON_PARTIAL_TXN = TRUE) — TLC reports a
\* counterexample to ReadYourOwnWrites.

EXTENDS RetryableInternalTxnRYOW

\* Keep the state space tight; the model is intentionally small.
StateConstraint == TRUE

=========================================================================
