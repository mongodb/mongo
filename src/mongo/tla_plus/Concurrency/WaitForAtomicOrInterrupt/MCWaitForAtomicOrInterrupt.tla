------------------------ MODULE MCWaitForAtomicOrInterrupt --------------------------------------
\* Model-checking module for WaitForAtomicOrInterrupt.

EXTENDS WaitForAtomicOrInterrupt

(**************************************************************************************************)
(* State constraints. Bound atomic flips so the search is finite under fair scheduling.            *)
(**************************************************************************************************)

FlipBound == atomicFlips <= MaxAtomicFlips

(**************************************************************************************************)
(* Bait invariants -- not asserted, only documented for parity with neighbouring specs. The        *)
(* useful counter-example for the bug config (InterruptIsResponsive = FALSE) is produced by        *)
(* the liveness property InterruptIsResponsiveProp, not by a safety invariant.                     *)
(**************************************************************************************************)

BaitNoInterruptExit ==
    \A t \in Threads : exitReason[t] # "interrupt"

=================================================================================================
