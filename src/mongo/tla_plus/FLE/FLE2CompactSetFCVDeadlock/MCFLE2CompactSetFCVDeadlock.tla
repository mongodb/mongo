----------------- MODULE MCFLE2CompactSetFCVDeadlock --------------------------
\* Model-checking module for FLE2CompactSetFCVDeadlock.
\*
\* The default configuration (MCFLE2CompactSetFCVDeadlock.cfg) sets
\*   CompactAcquiresInOrder = TRUE
\* which models the FIX -- the wait-for graph is acyclic and DeadlockFree holds.
\*
\* The bug configuration (MCFLE2CompactSetFCVDeadlock_Bug.cfg) sets
\*   CompactAcquiresInOrder = FALSE
\* which models the buggy code path described in SERVER-122159. TLC produces a
\* counter-example trace whose final state has setFCV waiting on Global (held
\* by Compact in IX) and Compact waiting on MDTB (held by setFCV in S) -- the
\* cycle reported in the ticket.

EXTENDS FLE2CompactSetFCVDeadlock

(******************************************************************************)
(* State constraints.                                                         *)
(******************************************************************************)

\* The state space is finite (two threads, two resources, two modes,
\* bounded pc states, and threads can only re-acquire after Release wipes
\* held[t]). No state constraint is needed; this exists so configs that want
\* to assert one can reference it.
NoExtraConstraint == TRUE

(******************************************************************************)
(* Counterexamples / baits.                                                   *)
(******************************************************************************)

\* Bait: the bug-trace must reach the cycle state -- if the model is correct,
\* this should fail (TLC reports a violation, which is the counter-example we
\* want) when CompactAcquiresInOrder = FALSE, and should hold (no cycle ever)
\* when CompactAcquiresInOrder = TRUE.
BaitNoCycleEverReached == ~HasCycle2

================================================================================
