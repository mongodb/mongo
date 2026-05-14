\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------- MODULE PagematdSegmentReconnectRace -----------------------------------------
\* This specification models the race between the page materializer daemon (pagematd) subscribing to
\* a log server's ReadLog stream and the cluster metadata service (CMS) registering a new log
\* segment.
\*
\* SERVER-126377 / BF-43088. On reconnect to a new log server, pagematd rebuilds the ReadLog
\* subscription filter from its in-memory "known segments" view. That view is populated only by
\* (a) the initial CMS bootstrap and (b) seal records observed on the live stream; it is never
\* refreshed on reconnect. If CMS registers a new segment in the window between pagematd's
\* disconnect and the new ReadLog stream being established, the new segment is absent from the
\* filter, no seal is delivered through it, and pagematd's known view never converges. The
\* materialization frontier on the page server freezes at the prior segment boundary.
\*
\* The spec models three actors:
\*   - CMS:        registers segments. Its state is the authoritative "ground truth" segment list,
\*                 cmsSegments, which grows monotonically.
\*   - LogServer:  publishes seal records (logSealed). A seal reaches pagematd only if pagematd is
\*                 currently subscribed to that segment AND connected.
\*   - Pagematd:   carries (i) a connection bit pmdConnected, (ii) an in-memory view
\*                 pmdKnownSegments that grows monotonically from CMS bootstrap and observed seals,
\*                 (iii) the active ReadLog subscription filter pmdSubscription, (iv) the
\*                 materialized-segments set pmdTracked.
\*
\* Two configurations:
\*
\*   BugMode = TRUE  (current code path):
\*     PmdReconnect sets pmdSubscription' = pmdKnownSegments without refreshing pmdKnownSegments
\*     from CMS. Any segment registered while disconnected (or during the reconnect itself) is
\*     missing from the new filter and is permanently invisible.
\*
\*   BugMode = FALSE (proposed fix):
\*     PmdReconnect first refreshes pmdKnownSegments from cmsSegments, then sets the subscription.
\*     Any segment registered up to the moment of reconnect is captured.
\*
\* The liveness invariant EverySegmentEventuallySeen asserts that every segment ever registered in
\* CMS is eventually in pmdTracked. TLC produces a counterexample under BugMode = TRUE matching the
\* BF-43088 trace, and verifies the invariant under BugMode = FALSE.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Disagg/PagematdSegmentReconnectRace

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Segments,       \* Finite set of segment identifiers CMS may register.
    MaxReconnects,  \* Bound on reconnect cycles, keeping the model finite.
    BugMode         \* TRUE  => current buggy behavior: reconnect uses stale local view.
                    \* FALSE => fixed behavior: reconnect refreshes local view from CMS first.

ASSUME Cardinality(Segments) > 0
ASSUME MaxReconnects \in 0..10
ASSUME BugMode \in BOOLEAN

(* CMS variables *)
VARIABLE cmsSegments        \* Set of segments registered in CMS. Monotonically grows.

(* Log server variables *)
VARIABLE logSealed          \* Set of segments whose seal record has been published on the log
                            \* stream. Monotonically grows.

(* Pagematd variables *)
VARIABLE pmdConnected       \* TRUE iff pagematd holds a live ReadLog subscription.
VARIABLE pmdKnownSegments   \* In-memory segment view. Updated on bootstrap, on observed seals, and
                            \* on reconnect-time CMS refresh (fix mode only). Survives disconnects.
VARIABLE pmdSubscription    \* The segment filter installed on the active ReadLog stream. Snapshot
                            \* of pmdKnownSegments at the moment of (re)connect. Empty when
                            \* disconnected.
VARIABLE pmdTracked         \* Set of segments pagematd has actually seen a seal for. This is the
                            \* materialization frontier set.
VARIABLE reconnectCount     \* Ancillary bound on reconnect cycles.

vars == <<cmsSegments, logSealed, pmdConnected, pmdKnownSegments, pmdSubscription, pmdTracked,
          reconnectCount>>

(**************************************************************************************************)
(* Type invariant.                                                                                *)
(**************************************************************************************************)

TypeOK ==
    /\ cmsSegments       \subseteq Segments
    /\ logSealed         \subseteq Segments
    /\ pmdKnownSegments  \subseteq Segments
    /\ pmdSubscription   \subseteq Segments
    /\ pmdTracked        \subseteq Segments
    /\ pmdConnected      \in BOOLEAN
    /\ reconnectCount    \in 0..MaxReconnects

(**************************************************************************************************)
(* Initial state. CMS empty, no seals, pagematd disconnected with empty in-memory view. The first  *)
(* connect handles bootstrap; under both BugMode values, the bootstrap refreshes from CMS, so the  *)
(* difference only appears on the SECOND and later connects (i.e., reconnects). This matches the   *)
(* code: bootstrap goes through checkAndRefreshConfig; reconnect does not.                         *)
(**************************************************************************************************)

Init ==
    /\ cmsSegments      = {}
    /\ logSealed        = {}
    /\ pmdConnected     = FALSE
    /\ pmdKnownSegments = {}
    /\ pmdSubscription  = {}
    /\ pmdTracked       = {}
    /\ reconnectCount   = 0

(**************************************************************************************************)
(* Actions.                                                                                       *)
(**************************************************************************************************)

\* CMS registers a new segment. Authoritative add.
\*
\* Gated on having at least one reconnect cycle of slack remaining (reconnectCount + 1 <
\* MaxReconnects). This guarantees pagematd always has budget to catch up via a fresh reconnect,
\* which under BugMode = FALSE is sufficient to keep the liveness invariant satisfiable. Without
\* this guard, TLC could explore "register after reconnect budget is exhausted" paths and trip
\* the invariant in the FIX configuration for the wrong reason (insufficient model budget rather
\* than missed-segment behavior).
CmsRegisterSegment(s) ==
    /\ s \in Segments
    /\ s \notin cmsSegments
    /\ reconnectCount + 1 < MaxReconnects
    /\ cmsSegments' = cmsSegments \union {s}
    /\ UNCHANGED <<logSealed, pmdConnected, pmdKnownSegments, pmdSubscription, pmdTracked,
                   reconnectCount>>

\* The log server publishes a seal record for a registered segment.
LogPublishSeal(s) ==
    /\ s \in cmsSegments
    /\ s \notin logSealed
    /\ logSealed' = logSealed \union {s}
    /\ UNCHANGED <<cmsSegments, pmdConnected, pmdKnownSegments, pmdSubscription, pmdTracked,
                   reconnectCount>>

\* Pagematd disconnects from the current log server. Subscription torn down. In-memory view and
\* tracked set persist. Gated on having reconnect budget so the system can always re-establish.
\*
\* The model treats disconnect as the ONLY trigger for a fresh CMS view. Production has additional
\* refresh paths (watchdogs, periodic CMS pings) that this spec abstracts into "eventually a
\* disconnect-reconnect cycle happens when the materializer is behind". WF on this action under
\* the gap-precondition models that watchdog.
PmdDisconnect ==
    /\ pmdConnected = TRUE
    /\ reconnectCount < MaxReconnects
    /\ pmdConnected'    = FALSE
    /\ pmdSubscription' = {}
    /\ UNCHANGED <<cmsSegments, logSealed, pmdKnownSegments, pmdTracked, reconnectCount>>

\* Returns TRUE iff pagematd is missing at least one CMS-registered segment from its subscription.
\* Used to predicate the watchdog-style fairness on disconnect: if pagematd is behind, eventually
\* it tears down and rebuilds.
GapExists == pmdConnected /\ \E s \in cmsSegments : s \notin pmdSubscription

\* Pagematd (re)connects to a log server. Two behaviors:
\*
\*   BugMode = TRUE: only the FIRST connect refreshes from CMS (bootstrap). On subsequent
\*     reconnects, pmdKnownSegments is left as-is and the new subscription is taken directly from
\*     it. Any segment registered while disconnected is permanently outside the filter.
\*
\*   BugMode = FALSE: every (re)connect refreshes pmdKnownSegments from cmsSegments first.
\*
\* The "first vs subsequent" distinction is captured by reconnectCount = 0.
PmdReconnect ==
    /\ pmdConnected = FALSE
    /\ reconnectCount < MaxReconnects
    /\ pmdConnected' = TRUE
    /\ pmdKnownSegments' =
        IF BugMode /\ reconnectCount > 0
            THEN pmdKnownSegments
            ELSE cmsSegments
    /\ pmdSubscription' =
        IF BugMode /\ reconnectCount > 0
            THEN pmdKnownSegments
            ELSE cmsSegments
    /\ reconnectCount' = reconnectCount + 1
    /\ UNCHANGED <<cmsSegments, logSealed, pmdTracked>>

\* Pagematd receives a seal record from the log stream for a segment in its subscription. This
\* grows pmdTracked AND pmdKnownSegments (the seal teaches pagematd that segment exists too, which
\* matters for any later reconnect that re-uses pmdKnownSegments).
PmdReceiveSeal(s) ==
    /\ pmdConnected = TRUE
    /\ s \in pmdSubscription
    /\ s \in logSealed
    /\ s \notin pmdTracked
    /\ pmdTracked'        = pmdTracked        \union {s}
    /\ pmdKnownSegments'  = pmdKnownSegments  \union {s}
    /\ UNCHANGED <<cmsSegments, logSealed, pmdConnected, pmdSubscription, reconnectCount>>

Next ==
    \/ \E s \in Segments : CmsRegisterSegment(s)
    \/ \E s \in Segments : LogPublishSeal(s)
    \/ PmdDisconnect
    \/ PmdReconnect
    \/ \E s \in Segments : PmdReceiveSeal(s)

(**************************************************************************************************)
(* Fairness. Weak fairness on the "progress" actions so liveness has teeth. Disconnect is NOT      *)
(* fair: we do not require disconnects to keep happening, only that if a disconnect occurs the     *)
(* system eventually heals.                                                                        *)
(**************************************************************************************************)

Fairness ==
    /\ \A s \in Segments : WF_vars(LogPublishSeal(s))
    /\ \A s \in Segments : WF_vars(PmdReceiveSeal(s))
    /\ WF_vars(PmdReconnect)
    /\ WF_vars(GapExists /\ PmdDisconnect)

Spec == Init /\ [][Next]_vars /\ Fairness

(**************************************************************************************************)
(* Safety invariants.                                                                             *)
(**************************************************************************************************)

\* Tracked segments must have actually been registered in CMS.
TrackedSubsetOfCms == pmdTracked \subseteq cmsSegments

\* The known view never goes beyond what CMS actually holds (anti-phantom).
KnownSubsetOfCms == pmdKnownSegments \subseteq cmsSegments

\* A live subscription is always a subset of the known view (it was built from it).
SubscriptionSubsetOfKnown == pmdSubscription \subseteq pmdKnownSegments

(**************************************************************************************************)
(* Liveness invariant.                                                                            *)
(*                                                                                                *)
(* Every segment registered in CMS is eventually tracked by pagematd. Under BugMode = TRUE, TLC    *)
(* yields a counterexample of the form:                                                           *)
(*                                                                                                *)
(*    1. CmsRegisterSegment(s1)                                                                   *)
(*    2. PmdReconnect (first connect, refreshes from CMS) -> pmdSubscription = {s1}               *)
(*    3. LogPublishSeal(s1), PmdReceiveSeal(s1) -> pmdTracked = {s1}                              *)
(*    4. PmdDisconnect -> pmdSubscription = {}, pmdKnownSegments = {s1}                           *)
(*    5. CmsRegisterSegment(s2)                                                                   *)
(*    6. PmdReconnect (subsequent connect; BugMode skips refresh) ->                              *)
(*         pmdSubscription = pmdKnownSegments = {s1}                                              *)
(*    7. LogPublishSeal(s2). PmdReceiveSeal(s2) is disabled forever                               *)
(*         (s2 \notin pmdSubscription, and reconnect budget exhausted).                           *)
(*       => s2 \in cmsSegments but s2 \notin pmdTracked, forever.                                 *)
(**************************************************************************************************)

EverySegmentEventuallySeen ==
    \A s \in Segments : [] ((s \in cmsSegments) => <>(s \in pmdTracked))

====================================================================================================
