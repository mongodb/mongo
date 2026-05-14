
\* Copyright 2025 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE MCRangeDeletionRenameRace ------------------------------

EXTENDS RangeDeletionRenameRace

(**************************************************************************************************)
(* Counterexample bait: assert the observer never reaches DECIDED so that TLC produces a trace    *)
(* showing how the bug interleaving completes. Wired into the .cfg only when desired.             *)
(**************************************************************************************************)

BaitObserverNeverDecides ==
    observerState # "DECIDED"

====================================================================================================
