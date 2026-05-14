\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------------- MODULE MCConfigServerTransitionOrphan ---------------------------------
\* Model-checking instance for ConfigServerTransitionOrphan.tla.
\*
\* Two configurations are checked from the same module:
\*
\*   * Green path  (MCConfigServerTransitionOrphan.cfg):
\*       AllowDropBeforeMarkReady = FALSE
\*     The drop of `config.rangeDeletions` is gated on all in-flight migrations having reached
\*     `Ready` or `Cleared`. Safety + liveness invariants hold.
\*
\*   * Bug path    (MCConfigServerTransitionOrphan_Bug.cfg):
\*       AllowDropBeforeMarkReady = TRUE
\*     The drop is unconditional, modelling current master (HEAD post-SERVER-103990). TLC produces
\*     a counterexample to `NoRangePermanentlyStuck` and to `EveryPendingRangeEventuallyCleared`,
\*     reproducing SERVER-125663.

EXTENDS ConfigServerTransitionOrphan

(**************************************************************************************************)
(* Symmetry over Ranges -- ranges are interchangeable for the purposes of this race.              *)
(**************************************************************************************************)
Symmetry == Permutations(Ranges)

(**************************************************************************************************)
(* Counterexample-baiting invariants. Run with `-invariant` to surface specific reachability      *)
(* claims; not part of the default `cfg` invariant list.                                          *)
(**************************************************************************************************)

\* TLC will produce a trace showing the bug as soon as any range reaches StuckPending.
BaitNoStuckPending == \A r \in Ranges : rangeState[r] # RangeStuckPending

\* TLC will produce a trace showing the drop happening with an in-memory task in `Registered`.
BaitNoConcurrentDrop ==
    ~(rangeDeletionsColl = RangeDeletionsCollDropped
      /\ \E r \in Ranges : rangeState[r] = RangeRegistered)

====================================================================================================
