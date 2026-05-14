------------------ MODULE MCOplogProviderShutdownLiveness ------------------
\* Model-checking module for OplogProviderShutdownLiveness.

EXTENDS OplogProviderShutdownLiveness

(**************************************************************************************************)
(* State constraints. None needed; the state space is already tiny.                                *)
(**************************************************************************************************)

(**************************************************************************************************)
(* Counterexample baits, useful when toggling the configuration to the bug ordering.               *)
(**************************************************************************************************)

\* In the buggy ordering with an uninterruptible resource wait we expect
\* ShutdownEventuallyExits to be falsified. Negating it as a bait invariant
\* gives an explicit trace.
BaitProviderEventuallyExits == providerState # "exited"

============================================================================
