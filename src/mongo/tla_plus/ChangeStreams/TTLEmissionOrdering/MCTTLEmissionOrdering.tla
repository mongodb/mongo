\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

-------------------------- MODULE MCTTLEmissionOrdering --------------------------
\* Model-checking module for TTLEmissionOrdering.
\*
\* Provides finite-state constants and a state constraint so TLC can
\* exhaustively explore the reachable state space in seconds rather than hours.
\*
\* Also defines two "bait" predicates that intentionally hold in the bug-mode
\* configurations, so the spec can demonstrate the model checker detects the
\* corresponding violations when the bug knobs are flipped on.

EXTENDS TTLEmissionOrdering

(**************************************************************************************************)
(* State constraint.                                                                              *)
(*                                                                                                *)
(* Bound the oplog and stream_buffer length. Without this TLC will explore unbounded sequences   *)
(* once it discovers any cycle that grows them.                                                  *)
(**************************************************************************************************)
StateBound ==
    /\ Len(oplog) <= 6
    /\ Len(stream_buffer) <= 6
    /\ Len(observed) <= 6

(**************************************************************************************************)
(* Counterexample bait predicates.                                                                *)
(*                                                                                                *)
(* Enabling these as INVARIANTs in the .cfg should produce a counterexample trace, proving the   *)
(* spec is wired up correctly to detect the relevant scenarios. They are commented out in the   *)
(* shipped .cfg so the headline run is green.                                                    *)
(**************************************************************************************************)

\* True whenever the consumer has observed at least one delete event.
\* Useful as a bait to confirm the model reaches the interesting states.
BaitDeleteObserved ==
    ~ \E i \in 1..Len(observed) : observed[i].op = "delete"

\* True whenever no doc has progressed to ttl_deleted.
\* Useful as a bait to verify TTL_MONITOR actually runs.
BaitNoTTLDelete ==
    \A d \in Docs : collection[d].state # "ttl_deleted"

================================================================================
