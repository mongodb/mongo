------------------------ MODULE MCCheckpointOplogTableVisibility ------------------------
\* Model-checking module for CheckpointOplogTableVisibility.
\*
\* Two configs ship alongside this module:
\*   MCCheckpointOplogTableVisibility.cfg     - fix mode (BugMode = FALSE), expects
\*                                              NoUseOfMissingTable to hold.
\*   MCCheckpointOplogTableVisibility_bug.cfg - bug mode (BugMode = TRUE), expects
\*                                              NoUseOfMissingTable to fail with a
\*                                              counterexample that matches the ticket.

EXTENDS CheckpointOplogTableVisibility

(**************************************************************************************************)
(* State constraints. Bound the explored state space to a tractable size.                         *)
(**************************************************************************************************)

\* Cap exploration on the number of checkpoint installs so TLC terminates quickly.
CheckpointBound == lastCheckpointTs <= MaxTimestamp

\* Cap the number of distinct TableNotFound events we collect; one is enough to
\* prove the bug fires.
TableNotFoundBound == Cardinality(tableNotFoundEvents) <= 1

(**************************************************************************************************)
(* Counterexamples / bait invariants.                                                             *)
(**************************************************************************************************)

\* "Bait" invariant: assert that the system never reaches a state where the
\* in-memory catalog points at an ident but that ident's table has been dropped
\* from disk. Under BugMode = TRUE this is violated.
BaitNoMissingTable == NoUseOfMissingTable

\* Bait: there is never a TableNotFound event. Under BugMode = TRUE the applier's
\* read after a racing checkpoint install produces one.
BaitNoTableNotFound == NoTableNotFoundEvents

=================================================================================================
