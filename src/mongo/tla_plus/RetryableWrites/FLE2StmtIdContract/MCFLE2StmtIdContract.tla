------------------------------ MODULE MCFLE2StmtIdContract --------------------------------
\* Model-checking module for FLE2StmtIdContract.
\*
\* Run two configurations:
\*   MCFLE2StmtIdContract_buggy.cfg  -- reproduces SERVER-79952 (counterexamples expected
\*                                      on ClientStmtIdsPreserved, ClientStmtIdsUnique,
\*                                      and NoAuxOverwritesClientStmtId).
\*   MCFLE2StmtIdContract_fixed.cfg  -- validates the proposed fix; all invariants hold.

EXTENDS FLE2StmtIdContract

(**************************************************************************************************)
(* Constant overrides for TLC. Sequences cannot be assigned directly in a .cfg, so the MC         *)
(* module replaces them with operator-style overrides.                                            *)
(**************************************************************************************************)

MCClientStmtIds == <<1, 3>>
MCAuxPerOp == 2
MCMode == "buggy"

(**************************************************************************************************)
(* State constraints.                                                                             *)
(**************************************************************************************************)

\* Cap the history length so TLC terminates on misconfigurations.
HistoryBounded == Len(history) <= 64

(**************************************************************************************************)
(* Counterexamples.                                                                               *)
(**************************************************************************************************)

\* Bait invariant: if used as INVARIANT it should always be violated (forces TLC to print a
\* trace). Useful for sanity-checking the spec wiring.
BaitNoProgress == nextOp = 1

==========================================================================================
