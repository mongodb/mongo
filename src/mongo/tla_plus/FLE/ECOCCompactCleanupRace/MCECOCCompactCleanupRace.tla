------------------------------ MODULE MCECOCCompactCleanupRace ------------------------------------
\* Model-checking module for ECOCCompactCleanupRace.

EXTENDS ECOCCompactCleanupRace

(****************************************************************************************************)
(* State constraints.                                                                                *)
(****************************************************************************************************)

\* TLC state-space cap. Maintenance and user-write counts are bounded by constants already; this
\* additional bound on history-length keeps trace-driven counterexamples readable.
HistoryLengthBound == Len(history) < 24

(****************************************************************************************************)
(* Counterexample baits.                                                                             *)
(****************************************************************************************************)

\* Bait invariant: if model-checked with CatalogPinsClusteredIndex = FALSE, TLC produces the
\* race trace described in SERVER-126384. Flip CatalogPinsClusteredIndex to TRUE in the .cfg and
\* the invariant holds across the full state space.
BaitECOCAlwaysExists == ecocExists

===================================================================================================
