
\* Copyright 2025 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

-------------------------------- MODULE MCRangeDeletionsSecondaryNodes -----------------------------

EXTENDS RangeDeletionsSecondaryNodes

(**************************************************************************************************)
(* Counterexamples.                                                                               *)
(**************************************************************************************************)

\* Verify the query succeeds on some executions.
BaitQueryOk ==
    /\ queryState \notin {"DONE_OK"}

====================================================================================================
