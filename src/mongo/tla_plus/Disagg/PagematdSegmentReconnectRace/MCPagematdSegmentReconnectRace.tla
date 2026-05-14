---------------------------- MODULE MCPagematdSegmentReconnectRace ---------------------------------
\* This module defines PagematdSegmentReconnectRace.tla constants/constraints for model-checking.

EXTENDS PagematdSegmentReconnectRace

(**************************************************************************************************)
(* Symmetry. Segments are interchangeable for the bug under test, so permuting them yields         *)
(* equivalent states.                                                                              *)
(**************************************************************************************************)

Symmetry == Permutations(Segments)

(**************************************************************************************************)
(* State constraint. Bound the state space by capping the reconnect counter (already a CONSTANT)   *)
(* and disallowing more growth past the point at which all segments are registered AND all seals   *)
(* are published. After that, only pagematd progress matters.                                      *)
(**************************************************************************************************)

ConstraintBounded ==
    /\ reconnectCount <= MaxReconnects

(**************************************************************************************************)
(* Counterexamples (bait predicates). Negate one of these as an invariant in the .cfg to force     *)
(* TLC to print a trace exhibiting the named behavior.                                             *)
(**************************************************************************************************)

\* Bait: pagematd reconnects while some CMS-registered segment is not in its subscription. Under
\* BugMode = TRUE, this is the exact race window from SERVER-126377.
BaitReconnectMissesSegment ==
    ~ (\E s \in cmsSegments : pmdConnected = TRUE /\ s \notin pmdSubscription)

\* Bait: every registered segment ends up in pmdTracked. Negating this and running with BugMode
\* TRUE shows TLC immediately produces a healthy path; the liveness check is what surfaces the
\* bug.
BaitAllSegmentsTracked == cmsSegments # pmdTracked

====================================================================================================
