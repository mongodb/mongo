---------------------- MODULE MCCMSPSELookupTimeout ------------------------
\* Model-checking constants/constraints for CMSPSELookupTimeout.tla.
\*
\* Two cfgs share this module:
\*   - MCCMSPSELookupTimeout.cfg     — fix config, TimeoutEnabled = TRUE,
\*                                     checks liveness + safety, expected to
\*                                     pass.
\*   - MCCMSPSELookupTimeout_bug.cfg — pre-fix config, TimeoutEnabled = FALSE,
\*                                     liveness expected to fail with a
\*                                     counter-example showing an infinite
\*                                     TickWhileBlocked loop on a slow peer.

EXTENDS CMSPSELookupTimeout

(* Symmetry across interchangeable peers cuts the state space sharply. *)
Symmetry == Permutations(Peers)

(* Bound the elapsed clock so the state space is finite even when the
   deadline is disabled (bug cfg). With elapsed capped at MaxElapsed the
   model still surfaces the infinite-block as a stuttering trace because
   TickWhileBlocked stops being enabled once the cap is hit, leaving the
   lookup parked in PhaseReading with no progress towards PhaseDone — the
   shape of the counter-example we want. *)
MaxElapsed == AttemptBudget + 2
ElapsedBounded == \A p \in Peers : elapsed[p] <= MaxElapsed

(* Total attempts ceiling, mirrors the production retry policy. *)
AttemptsBounded == \A p \in Peers : attempts[p] <= MaxRetries + 2

StateConstraint ==
    /\ ElapsedBounded
    /\ AttemptsBounded
    /\ timeoutCount <= (MaxRetries + 1) * Cardinality(Peers) + 2

(* Bait: an attempt has been opened against a slow peer. Useful for
   exercising the slow-peer paths during interactive debugging. *)
BaitSlowOpened ==
    ~(\E p \in Peers : peerKind[p] = PeerSlow /\ phase[p] = PhaseReading)

(* Bait: the timeout counter has advanced at least once. Disabled by
   default; flip on with -view if you want TLC to halt the moment the
   metric is observed firing. *)
BaitTimeoutObserved == timeoutCount = 0

============================================================================
