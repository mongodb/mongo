\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------- MODULE StandbyStepUpRace --------------------------------
\* Models the standby loop / becomePrimary() race observed during step-up in the
\* disaggregated replication coordinator. See SERVER-126278.
\*
\* The setup: stepUpIfEligible() runs the following sequence on a worker thread:
\*
\*   1. cancelStateMachine()        \* stop any active state-machine processing
\*   2. stateMachineJoin()          \* drain pending state-machine work (joins active standby loop)
\*   3. acquire RSTL (and others)   \* this can block / take measurable time
\*   4. accept(kStepUp)             \* enqueue/process the StepUp transition
\*   5. becomePrimary()             \* construct primary log writer, etc.
\*
\* Concurrently, a separate state-machine queue can deliver an
\* accept(kDisconnected) callback on a *different* thread. That callback runs
\* reconnectAsSecondary(), which starts a fresh standby loop. The bug is that
\* step 2 only joins state-machine work that already exists; an accept(kDisconnected)
\* enqueued AFTER stateMachineJoin() but BEFORE accept(kStepUp) reaches
\* becomePrimary() will spin up a new standby loop that races with becomePrimary().
\* When becomePrimary() tears down or rebinds the gRPC stream that the new
\* standby loop holds, the standby loop's first ClientReader::Read() trips the
\* fatal CallOpRecvInitialMetadata assertion inside gRPC.
\*
\* The fix being modelled: once stepUpIfEligible() has committed to performing
\* a step-up, no accept(kDisconnected)/reconnectAsSecondary() may start a new
\* standby loop until becomePrimary() has run to completion. Equivalently, an
\* active standby loop and an in-flight becomePrimary() are mutually exclusive.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Workers   \* Set of worker thread identifiers that can run callbacks.

\* The substrate role on the node.
CONSTANTS Secondary,        \* Node is acting as a secondary; standby loop runs.
          BecomingPrimary,  \* Step-up sequence is mid-flight, between RSTL acquired and becomePrimary done.
          Primary           \* Node is fully primary; no standby loop.

\* The lifecycle of the standby loop itself.
CONSTANTS StandbyIdle,      \* No standby loop is running.
          StandbyRunning,   \* A standby loop has been started; its first ClientReader::Read() has not yet returned.
          StandbyJoined     \* The standby loop has been cancelled and joined (terminal for that instance).

\* Pending callbacks that the state machine can dispatch.
CONSTANTS Disconnected, StepUp

VARIABLES role,            \* element of {Secondary, BecomingPrimary, Primary}
          standby,         \* element of {StandbyIdle, StandbyRunning, StandbyJoined}
          stepUpHolder,    \* Worker currently driving stepUpIfEligible(), or NONE.
          stepUpPhase,     \* "idle" | "joined" | "rstlHeld" | "primary"
          callbackQueue,   \* Sequence of pending callbacks: each is [kind |-> Disconnected|StepUp, worker |-> w].
          grpcFatal,       \* TRUE once the race has tripped the gRPC fatal assertion.
          fixEnabled       \* TRUE = fix in place (standby loop creation gated on step-up); FALSE = bug cfg.

vars == <<role, standby, stepUpHolder, stepUpPhase, callbackQueue, grpcFatal, fixEnabled>>

NONE == "NONE"

-----------------------------------------------------------------------------
\* Helpers.
-----------------------------------------------------------------------------

\* TRUE if a worker is currently executing the step-up critical section that
\* covers RSTL-acquisition through becomePrimary().
StepUpInFlight ==
    /\ stepUpHolder /= NONE
    /\ stepUpPhase \in {"joined", "rstlHeld"}

\* TRUE once becomePrimary() has run.
BecomePrimaryDone == stepUpPhase = "primary"

\* TRUE if the standby loop's gRPC stream is in a state where a concurrent
\* becomePrimary() teardown will fatal it. In the real system, this is the
\* window between starting the standby loop and the first Read() returning,
\* during which CallOpRecvInitialMetadata is bound to the call. We model that
\* as: standby is StandbyRunning AND becomePrimary() either has run or is
\* running concurrently on another worker.
StandbyExposedToFatal ==
    /\ standby = StandbyRunning
    /\ role = BecomingPrimary

-----------------------------------------------------------------------------
\* Initial state.
-----------------------------------------------------------------------------

Init ==
    /\ role = Secondary
    /\ standby = StandbyRunning      \* a standby loop is running on a secondary
    /\ stepUpHolder = NONE
    /\ stepUpPhase = "idle"
    /\ callbackQueue = <<>>
    /\ grpcFatal = FALSE
    /\ fixEnabled \in BOOLEAN

-----------------------------------------------------------------------------
\* External events that enqueue state-machine callbacks.
\*
\* A network blip can enqueue accept(kDisconnected). A replSetStepUp command or
\* election win can enqueue accept(kStepUp). Real subsystems can enqueue either
\* at any moment; we permit interleavings inside fairness guards.
-----------------------------------------------------------------------------

EnqueueDisconnected(w) ==
    /\ Len(callbackQueue) < 3   \* state-bound for the model checker
    /\ callbackQueue' = Append(callbackQueue, [kind |-> Disconnected, worker |-> w])
    /\ UNCHANGED <<role, standby, stepUpHolder, stepUpPhase, grpcFatal, fixEnabled>>

EnqueueStepUp(w) ==
    /\ Len(callbackQueue) < 3
    /\ stepUpHolder = NONE
    /\ callbackQueue' = Append(callbackQueue, [kind |-> StepUp, worker |-> w])
    /\ UNCHANGED <<role, standby, stepUpHolder, stepUpPhase, grpcFatal, fixEnabled>>

-----------------------------------------------------------------------------
\* Step-up sequence on a worker thread, modelled in phases so that a separate
\* worker can interleave a Disconnected dispatch between them.
-----------------------------------------------------------------------------

\* Phase 1: stepUpIfEligible() picks up the StepUp callback and runs
\* cancelStateMachine()+stateMachineJoin(). Whatever standby loop existed at
\* this moment is joined.
StepUpJoin(w) ==
    /\ stepUpHolder = NONE
    /\ stepUpPhase = "idle"
    /\ Len(callbackQueue) > 0
    /\ Head(callbackQueue).kind = StepUp
    /\ stepUpHolder' = w
    /\ stepUpPhase' = "joined"
    /\ callbackQueue' = Tail(callbackQueue)
    /\ standby' = StandbyJoined
    /\ role' = BecomingPrimary
    /\ UNCHANGED <<grpcFatal, fixEnabled>>

\* Phase 2: acquire RSTL (and any other locks). Nothing about the standby
\* state changes here, but time passes during which other workers can run.
StepUpAcquireRSTL(w) ==
    /\ stepUpHolder = w
    /\ stepUpPhase = "joined"
    /\ stepUpPhase' = "rstlHeld"
    /\ UNCHANGED <<role, standby, stepUpHolder, callbackQueue, grpcFatal, fixEnabled>>

\* Phase 3: accept(kStepUp) reaches becomePrimary(). If the fix is enabled and
\* a standby loop is running concurrently, this is the moment the contract
\* says cannot happen; under the bug configuration we record the fatal.
StepUpBecomePrimary(w) ==
    /\ stepUpHolder = w
    /\ stepUpPhase = "rstlHeld"
    /\ stepUpPhase' = "primary"
    /\ role' = Primary
    /\ grpcFatal' = (grpcFatal \/ StandbyExposedToFatal)
    /\ UNCHANGED <<standby, stepUpHolder, callbackQueue, fixEnabled>>

\* Phase 4: step-up worker releases the role. Returning the holder to NONE
\* makes the spec re-enterable for liveness checking.
StepUpRelease(w) ==
    /\ stepUpHolder = w
    /\ stepUpPhase = "primary"
    /\ stepUpHolder' = NONE
    /\ stepUpPhase' = "idle"
    /\ UNCHANGED <<role, standby, callbackQueue, grpcFatal, fixEnabled>>

-----------------------------------------------------------------------------
\* Disconnected callback: reconnectAsSecondary() starts a fresh standby loop.
\*
\* fixEnabled = FALSE  -- the buggy world: a Disconnected callback can fire
\*                        even while StepUpInFlight, racing becomePrimary().
\* fixEnabled = TRUE   -- the fix: while a step-up is in flight, a Disconnected
\*                        callback either defers or runs only after becomePrimary()
\*                        has returned (i.e. while role /= BecomingPrimary).
-----------------------------------------------------------------------------

DispatchDisconnected(w) ==
    /\ Len(callbackQueue) > 0
    /\ Head(callbackQueue).kind = Disconnected
    /\ callbackQueue' = Tail(callbackQueue)
    /\ IF fixEnabled
       \* The fix gates Disconnected dispatch on "no in-flight step-up". The
       \* gate is checked atomically with picking up the callback, modelled
       \* here by tying the enabling clause to the role variable.
       THEN /\ role /= BecomingPrimary
            /\ standby' = StandbyRunning
            /\ UNCHANGED <<role, stepUpHolder, stepUpPhase, grpcFatal, fixEnabled>>
       \* No gate: a Disconnected callback can land mid-step-up.
       ELSE /\ standby' = StandbyRunning
            /\ UNCHANGED <<role, stepUpHolder, stepUpPhase, grpcFatal, fixEnabled>>

-----------------------------------------------------------------------------
\* Next state relation.
-----------------------------------------------------------------------------

Next ==
    \/ \E w \in Workers : EnqueueStepUp(w)
    \/ \E w \in Workers : EnqueueDisconnected(w)
    \/ \E w \in Workers : StepUpJoin(w)
    \/ \E w \in Workers : StepUpAcquireRSTL(w)
    \/ \E w \in Workers : StepUpBecomePrimary(w)
    \/ \E w \in Workers : StepUpRelease(w)
    \/ \E w \in Workers : DispatchDisconnected(w)

Spec ==
    /\ Init
    /\ [][Next]_vars
    \* Weak fairness: any pending step-up phase eventually advances.
    /\ \A w \in Workers : WF_vars(StepUpJoin(w))
    /\ \A w \in Workers : WF_vars(StepUpAcquireRSTL(w))
    /\ \A w \in Workers : WF_vars(StepUpBecomePrimary(w))
    /\ \A w \in Workers : WF_vars(StepUpRelease(w))

-----------------------------------------------------------------------------
\* Type / sanity invariants.
-----------------------------------------------------------------------------

TypeOK ==
    /\ role \in {Secondary, BecomingPrimary, Primary}
    /\ standby \in {StandbyIdle, StandbyRunning, StandbyJoined}
    /\ stepUpHolder \in (Workers \union {NONE})
    /\ stepUpPhase \in {"idle", "joined", "rstlHeld", "primary"}
    /\ grpcFatal \in BOOLEAN
    /\ fixEnabled \in BOOLEAN
    /\ \A i \in 1..Len(callbackQueue) :
        /\ callbackQueue[i].kind \in {Disconnected, StepUp}
        /\ callbackQueue[i].worker \in Workers

\* If a step-up holder exists, the phase must be non-idle, and vice versa.
HolderPhaseConsistent ==
    (stepUpHolder = NONE) <=> (stepUpPhase = "idle")

-----------------------------------------------------------------------------
\* Safety: the core invariant the bug violates.
\*
\* While the step-up is in flight (between stateMachineJoin() and
\* becomePrimary() returning), the standby loop must not be running.
\* Equivalently: the standby-loop "running" predicate and the
\* "becomePrimary in flight" predicate are mutually exclusive.
-----------------------------------------------------------------------------

NoConcurrentStandbyAndBecomePrimary ==
    \* role = BecomingPrimary is exactly the window from joined-through-becomePrimary.
    (role = BecomingPrimary) => (standby /= StandbyRunning)

\* No fatal gRPC assertion ever fires.
NoGrpcFatal == ~grpcFatal

\* Strict form: at most one of {standby running, step-up in flight} at a time.
StandbyAndStepUpExclusive ==
    ~(standby = StandbyRunning /\ StepUpInFlight)

-----------------------------------------------------------------------------
\* Liveness: step-up always completes if requested.
-----------------------------------------------------------------------------

StepUpEventuallyCompletes ==
    (\E i \in 1..Len(callbackQueue) : callbackQueue[i].kind = StepUp)
        ~> (role = Primary)

=================================================================================================
