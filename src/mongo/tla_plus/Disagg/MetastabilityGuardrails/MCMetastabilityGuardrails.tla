\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE MCMetastabilityGuardrails -------------------------
\* Model-checking module for MetastabilityGuardrails.
\*
\* Two .cfg files share this module:
\*   * MCMetastabilityGuardrails.cfg          -> GuardrailsEnabled = TRUE,
\*                                               EventuallyReturnsToHealthy holds.
\*   * MCMetastabilityGuardrailsBug.cfg       -> GuardrailsEnabled = FALSE,
\*                                               EventuallyReturnsToHealthy fails;
\*                                               TLC emits a metastable trace.

EXTENDS MetastabilityGuardrails

(**************************************************************************)
(* State constraints. Bound the queue and the request id so TLC's state   *)
(* graph stays tractable; the spec's qualitative liveness contract is     *)
(* independent of these specific numerical bounds.                        *)
(**************************************************************************)

QueueBound == Len(queue) <= QueueCapacity
RequestIdBound == nextRequestId <= 12
ClockBound == clock <= MaxClock

StateConstraint ==
    /\ QueueBound
    /\ RequestIdBound
    /\ ClockBound

(**************************************************************************)
(* Bait invariants. Useful for confirming the model exercises the         *)
(* expected states; flipping these to INVARIANT in the cfg generates a   *)
(* witness trace.                                                         *)
(**************************************************************************)

\* If this ever flips false, the spec produced a queue-full state.
BaitQueueNeverFull == QueueDepth < QueueCapacity

\* If this ever flips false, the breaker actually tripped.
BaitBreakerNeverOpens ==
    \A tgt \in Targets : breakerState[tgt] /= OPEN

================================================================================
