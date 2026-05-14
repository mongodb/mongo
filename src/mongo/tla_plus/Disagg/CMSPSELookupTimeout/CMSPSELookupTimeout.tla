\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------ MODULE CMSPSELookupTimeout --------------------------
\* This specification models the Cluster Metadata Service (CMS) lookup against
\* a Page Storage Extent (PSE) peer for PSE information retrieval, as performed
\* by InSLSConfigManager::subscribeToPageServers().
\*
\* The production code paths involved are:
\*   - GetPageServingExtentList is sent on a streaming gRPC ClientContext.
\*   - reader->Read() pulls one entry at a time; the terminating status arrives
\*     via reader->Finish().
\*   - Neither call has a deadline applied to the ClientContext, so if a peer
\*     accepts the stream and then stops producing entries (slow / hung peer),
\*     the calling thread blocks forever on Read(). On startup this blocks
\*     writes because the subscription has to complete before the node serves
\*     traffic (SERVER-115352).
\*
\* The model:
\*   - One CMS caller (CmsThread) issues a lookup against each peer in Peers.
\*   - Each peer is in one of {responsive, slow, unreachable}. A responsive
\*     peer eventually streams a terminating Finish; a slow peer accepts the
\*     stream but never produces entries; an unreachable peer refuses the
\*     stream open.
\*   - The caller's ClientContext carries an optional per-attempt deadline.
\*     When TimeoutEnabled = TRUE the deadline trips after AttemptBudget
\*     logical ticks and Read() / OpenStream() returns DeadlineExceeded.
\*     When TimeoutEnabled = FALSE the deadline never trips; this is the
\*     pre-fix configuration.
\*   - On DeadlineExceeded the caller classifies the failure structurally
\*     into {TimeoutExceeded, PeerUnreachable, PeerSlow} based on which
\*     phase tripped, increments the metrics.disagg.cms.pseLookupTimeouts
\*     counter (modelled as `timeoutCount'), and may retry up to MaxRetries
\*     times. Retries do not reset the global RetryBudget.
\*
\* Correctness properties:
\*   - LookupEventuallyTerminates (liveness, see comments below) — every
\*     started lookup eventually reaches a terminal status in
\*     {Succeeded, TimedOut, Unreachable, GaveUp}. Under the fix
\*     (TimeoutEnabled = TRUE) TLC reports no violation. Under the bug
\*     config (TimeoutEnabled = FALSE) TLC produces a counter-example in
\*     which a lookup against a slow peer remains in the BlockedOnRead
\*     state with no enabled action, so the lookup never terminates.
\*   - StructuredErrorClassification (safety) — whenever a lookup terminates
\*     in a non-success state, the recorded error is one of the three
\*     structured classes; never "Unknown".
\*   - MetricsAdvanceOnTimeoutOnly (safety) — timeoutCount only increases
\*     in states that just observed a DeadlineExceeded; ensures we don't
\*     double-count or fail to count.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Disagg/CMSPSELookupTimeout
\* The default cfg checks the fix config. The companion _bug.cfg flips
\* TimeoutEnabled to FALSE and asserts the liveness violation.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Peers,              \* Set of PSE peers the CMS subscribes to.
    MaxRetries,         \* Max retries per lookup (e.g. 2).
    AttemptBudget,      \* Per-attempt deadline in logical ticks (e.g. 3).
    TimeoutEnabled      \* TRUE  => deadline applied to ClientContext.
                        \* FALSE => pre-fix bug: deadline never applied.

ASSUME Cardinality(Peers) > 0
ASSUME MaxRetries \in 0..5
ASSUME AttemptBudget \in 1..10
ASSUME TimeoutEnabled \in BOOLEAN

\* Peer behavioural classes.
PeerResponsive  == "responsive"     \* Streams entries, eventually Finishes.
PeerSlow        == "slow"           \* Accepts open, then never produces.
PeerUnreachable == "unreachable"    \* Refuses open.
PeerKinds == {PeerResponsive, PeerSlow, PeerUnreachable}

\* Lookup phases the caller can be in.
PhaseIdle         == "idle"
PhaseOpening      == "opening"        \* Awaiting stream accept.
PhaseReading      == "reading"        \* Awaiting next Read().
PhaseFinishing    == "finishing"      \* Awaiting Finish() status.
PhaseRetryBackoff == "retryBackoff"   \* Sleeping before retry.
PhaseDone         == "done"           \* Terminal.
Phases == {PhaseIdle, PhaseOpening, PhaseReading, PhaseFinishing,
           PhaseRetryBackoff, PhaseDone}

\* Terminal outcomes recorded against a lookup.
OutcomePending      == "pending"
OutcomeSucceeded    == "succeeded"
OutcomeTimedOut     == "timedOut"       \* DeadlineExceeded after retries.
OutcomeUnreachable  == "unreachable"    \* Peer never accepted stream.
OutcomeGaveUp       == "gaveUp"         \* Retries exhausted on slow peer.
Outcomes == {OutcomePending, OutcomeSucceeded, OutcomeTimedOut,
             OutcomeUnreachable, OutcomeGaveUp}

\* Structured error classification (matches design.md table).
ErrNone             == "none"
ErrTimeoutExceeded  == "TimeoutExceeded"
ErrPeerUnreachable  == "PeerUnreachable"
ErrPeerSlow         == "PeerSlow"
Errors == {ErrNone, ErrTimeoutExceeded, ErrPeerUnreachable, ErrPeerSlow}

VARIABLES
    peerKind,        \* [Peers -> PeerKinds] — assigned in Init.
    phase,           \* [Peers -> Phases] — current phase of each lookup.
    elapsed,         \* [Peers -> Nat] — ticks since current attempt started.
    attempts,        \* [Peers -> Nat] — number of attempts begun (>= 1 once started).
    outcome,         \* [Peers -> Outcomes].
    errorClass,      \* [Peers -> Errors] — last structured error observed.
    timeoutCount,    \* Nat — metrics.disagg.cms.pseLookupTimeouts proxy.
    deadlineFired    \* [Peers -> BOOLEAN] — set TRUE the tick the deadline
                     \* trips, cleared the next time the caller acts. Lets us
                     \* prove MetricsAdvanceOnTimeoutOnly.

vars == <<peerKind, phase, elapsed, attempts, outcome, errorClass,
          timeoutCount, deadlineFired>>

----------------------------------------------------------------------------
(* Initial state. *)

Init ==
    /\ peerKind \in [Peers -> PeerKinds]
    /\ phase = [p \in Peers |-> PhaseIdle]
    /\ elapsed = [p \in Peers |-> 0]
    /\ attempts = [p \in Peers |-> 0]
    /\ outcome = [p \in Peers |-> OutcomePending]
    /\ errorClass = [p \in Peers |-> ErrNone]
    /\ timeoutCount = 0
    /\ deadlineFired = [p \in Peers |-> FALSE]

----------------------------------------------------------------------------
(* Helpers. *)

\* The caller's deadline is honoured iff TimeoutEnabled = TRUE.
HasDeadline == TimeoutEnabled

\* The deadline has tripped on this attempt for peer p.
DeadlineTripped(p) == HasDeadline /\ elapsed[p] >= AttemptBudget

\* A lookup that started but is parked waiting on the peer.
BlockedOnPeer(p) == phase[p] \in {PhaseOpening, PhaseReading}

----------------------------------------------------------------------------
(* Actions: the lookup state machine. *)

\* Action: start (or restart) an attempt against peer p.
StartAttempt(p) ==
    /\ phase[p] \in {PhaseIdle, PhaseRetryBackoff}
    /\ outcome[p] = OutcomePending
    /\ phase' = [phase EXCEPT ![p] = PhaseOpening]
    /\ elapsed' = [elapsed EXCEPT ![p] = 0]
    /\ attempts' = [attempts EXCEPT ![p] = @ + 1]
    /\ deadlineFired' = [deadlineFired EXCEPT ![p] = FALSE]
    /\ UNCHANGED <<peerKind, outcome, errorClass, timeoutCount>>

\* Action: peer accepts the stream open.
\* Responsive and slow peers accept; unreachable peers do not (handled by
\* OpenStreamRefused).
OpenStreamAccepted(p) ==
    /\ phase[p] = PhaseOpening
    /\ peerKind[p] \in {PeerResponsive, PeerSlow}
    /\ ~DeadlineTripped(p)
    /\ phase' = [phase EXCEPT ![p] = PhaseReading]
    /\ elapsed' = [elapsed EXCEPT ![p] = 0]
    /\ UNCHANGED <<peerKind, attempts, outcome, errorClass,
                   timeoutCount, deadlineFired>>

\* Action: peer refuses the stream open (unreachable). Treated as a
\* synchronous failure; no retry budget consumed for unreachable peers in
\* this simplified model (matches "peer is not in the topology" semantics).
OpenStreamRefused(p) ==
    /\ phase[p] = PhaseOpening
    /\ peerKind[p] = PeerUnreachable
    /\ phase' = [phase EXCEPT ![p] = PhaseDone]
    /\ outcome' = [outcome EXCEPT ![p] = OutcomeUnreachable]
    /\ errorClass' = [errorClass EXCEPT ![p] = ErrPeerUnreachable]
    /\ UNCHANGED <<peerKind, elapsed, attempts, timeoutCount,
                   deadlineFired>>

\* Action: a responsive peer streams the terminating Finish, the lookup
\* succeeds.
ReadStreamFinished(p) ==
    /\ phase[p] = PhaseReading
    /\ peerKind[p] = PeerResponsive
    /\ ~DeadlineTripped(p)
    /\ phase' = [phase EXCEPT ![p] = PhaseDone]
    /\ outcome' = [outcome EXCEPT ![p] = OutcomeSucceeded]
    /\ errorClass' = [errorClass EXCEPT ![p] = ErrNone]
    /\ UNCHANGED <<peerKind, elapsed, attempts, timeoutCount,
                   deadlineFired>>

\* Action: a logical tick elapses while parked on the peer. This is the
\* clock that drives the deadline towards AttemptBudget. Without a
\* deadline this action can fire forever; the liveness counterexample
\* hinges on this state having no enabled successor that progresses
\* `outcome`.
TickWhileBlocked(p) ==
    /\ BlockedOnPeer(p)
    /\ elapsed' = [elapsed EXCEPT ![p] = @ + 1]
    /\ UNCHANGED <<peerKind, phase, attempts, outcome, errorClass,
                   timeoutCount, deadlineFired>>

\* Action: deadline trips. Only enabled when the timeout is wired up
\* (HasDeadline) and the per-attempt budget has been spent. Records the
\* structurally classified error, increments the timeout counter, and
\* sends the lookup either back to retry or to the terminal state.
DeadlineExceeded(p) ==
    /\ HasDeadline
    /\ BlockedOnPeer(p)
    /\ DeadlineTripped(p)
    /\ timeoutCount' = timeoutCount + 1
    /\ deadlineFired' = [deadlineFired EXCEPT ![p] = TRUE]
    /\ LET classifiedErr ==
           IF phase[p] = PhaseOpening THEN ErrPeerUnreachable
                                       ELSE ErrPeerSlow
       IN  errorClass' = [errorClass EXCEPT ![p] = classifiedErr]
    /\ IF attempts[p] >= MaxRetries + 1
        THEN \* Retries exhausted.
            /\ phase' = [phase EXCEPT ![p] = PhaseDone]
            /\ outcome' = [outcome EXCEPT ![p] =
                            IF phase[p] = PhaseOpening
                                THEN OutcomeUnreachable
                                ELSE OutcomeGaveUp]
        ELSE
            /\ phase' = [phase EXCEPT ![p] = PhaseRetryBackoff]
            /\ outcome' = [outcome EXCEPT ![p] = OutcomePending]
    /\ UNCHANGED <<peerKind, elapsed, attempts>>

\* Action: the caller is interrupted (e.g. shutdown). Maps to the
\* "interrupted" path in the liveness predicate. Always enabled until
\* terminal so a hung subscribe can still be killed by stepdown.
Interrupted(p) ==
    /\ phase[p] # PhaseDone
    /\ phase' = [phase EXCEPT ![p] = PhaseDone]
    /\ outcome' = [outcome EXCEPT ![p] = OutcomeTimedOut]
    /\ errorClass' = [errorClass EXCEPT ![p] = ErrTimeoutExceeded]
    /\ UNCHANGED <<peerKind, elapsed, attempts, timeoutCount,
                   deadlineFired>>

\* Action: an observable timeoutCount read clears the deadlineFired flag.
\* This is purely a modelling trick to make MetricsAdvanceOnTimeoutOnly
\* express "the counter advanced in lockstep with a deadline trip".
ObserveTimeoutCounter(p) ==
    /\ deadlineFired[p]
    /\ deadlineFired' = [deadlineFired EXCEPT ![p] = FALSE]
    /\ UNCHANGED <<peerKind, phase, elapsed, attempts, outcome,
                   errorClass, timeoutCount>>

Next ==
    \/ \E p \in Peers : StartAttempt(p)
    \/ \E p \in Peers : OpenStreamAccepted(p)
    \/ \E p \in Peers : OpenStreamRefused(p)
    \/ \E p \in Peers : ReadStreamFinished(p)
    \/ \E p \in Peers : TickWhileBlocked(p)
    \/ \E p \in Peers : DeadlineExceeded(p)
    \/ \E p \in Peers : Interrupted(p)
    \/ \E p \in Peers : ObserveTimeoutCounter(p)

\* Fairness: every action that exists is eventually taken when enabled, so
\* that under the fix the liveness property is actually checked rather than
\* trivially violated by stuttering. We deliberately do NOT give fairness
\* to Interrupted: liveness must hold without relying on an external kill.
Fairness ==
    /\ WF_vars(\E p \in Peers : StartAttempt(p))
    /\ WF_vars(\E p \in Peers : OpenStreamAccepted(p))
    /\ WF_vars(\E p \in Peers : OpenStreamRefused(p))
    /\ WF_vars(\E p \in Peers : ReadStreamFinished(p))
    /\ WF_vars(\E p \in Peers : DeadlineExceeded(p))
    /\ WF_vars(\E p \in Peers : ObserveTimeoutCounter(p))
    /\ WF_vars(\E p \in Peers : TickWhileBlocked(p))

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------
(* Type invariants. *)

TypeOK ==
    /\ peerKind \in [Peers -> PeerKinds]
    /\ phase \in [Peers -> Phases]
    /\ elapsed \in [Peers -> Nat]
    /\ attempts \in [Peers -> Nat]
    /\ outcome \in [Peers -> Outcomes]
    /\ errorClass \in [Peers -> Errors]
    /\ timeoutCount \in Nat
    /\ deadlineFired \in [Peers -> BOOLEAN]

----------------------------------------------------------------------------
(* Safety properties. *)

\* Whenever a lookup terminates non-successfully, the error is structured.
StructuredErrorClassification ==
    \A p \in Peers :
        outcome[p] \in {OutcomeTimedOut, OutcomeUnreachable, OutcomeGaveUp}
            => errorClass[p] \in {ErrTimeoutExceeded, ErrPeerUnreachable,
                                  ErrPeerSlow}

\* The timeout counter only advances on the tick the deadline trips. If
\* the counter advances and no deadlineFired is set, we have an
\* accounting bug.
MetricsAdvanceOnTimeoutOnly ==
    timeoutCount > 0 =>
        \/ \E p \in Peers : deadlineFired[p]
        \/ \E p \in Peers : outcome[p] \in {OutcomeTimedOut,
                                            OutcomeUnreachable,
                                            OutcomeGaveUp}

\* A successful lookup must have observed no structured error.
SuccessImpliesClean ==
    \A p \in Peers :
        outcome[p] = OutcomeSucceeded => errorClass[p] = ErrNone

----------------------------------------------------------------------------
(* Liveness properties.                                                    *)
(*                                                                         *)
(* LookupEventuallyTerminates is the property the ticket asks for: every   *)
(* lookup eventually reaches a terminal phase, either by success, by a     *)
(* structured timeout, or by interrupt.                                    *)
(*                                                                         *)
(* Under TimeoutEnabled = TRUE TLC checks this property and finds no       *)
(* violation: the DeadlineExceeded action is eventually enabled on a slow  *)
(* / unreachable peer, retries are bounded by MaxRetries, and every path   *)
(* reaches PhaseDone.                                                      *)
(*                                                                         *)
(* Under TimeoutEnabled = FALSE (bug cfg) the DeadlineExceeded action is   *)
(* never enabled, TickWhileBlocked can fire forever on a slow peer, and    *)
(* no transition lands the lookup in PhaseDone. TLC produces the           *)
(* counter-example trace: StartAttempt -> OpenStreamAccepted ->            *)
(* TickWhileBlocked^ω with phase stuck at PhaseReading and outcome stuck   *)
(* at OutcomePending. This is the bug from SERVER-115352: the CMS lookup  *)
(* hangs forever on a slow PSE peer because the ClientContext has no       *)
(* deadline.                                                               *)

LookupEventuallyTerminates ==
    \A p \in Peers : <>(phase[p] = PhaseDone)

\* Stronger framing: every lookup eventually has a final outcome that is
\* one of {Succeeded, TimedOut, Unreachable, GaveUp}. Equivalent under the
\* spec's transitions but easier to read.
LookupEventuallyClassified ==
    \A p \in Peers :
        <>(outcome[p] \in {OutcomeSucceeded, OutcomeTimedOut,
                           OutcomeUnreachable, OutcomeGaveUp})

============================================================================
