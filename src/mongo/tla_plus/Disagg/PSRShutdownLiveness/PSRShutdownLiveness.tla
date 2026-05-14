\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE PSRShutdownLiveness ----------------------------------
\* Models the Page Server Reader (PSR) shutdown protocol for SERVER-113702.
\*
\* The PSR services page-read requests through a small pool of worker threads. Today there is
\* no clean shutdown path: when the server tears the PSR down, requests sitting in the
\* pending queue may be silently dropped, and in-flight requests on worker threads are not
\* given a chance to either complete or be canceled with a structured error. The reporting
\* client sees a connection-reset rather than ShutdownInProgress, retries hammer the dying
\* service, and orphaned requests leak resources.
\*
\* The spec models five shutdown phases that the design proposes:
\*
\*   1. Running       -- normal operation: clients enqueue, workers pick up and complete.
\*   2. StopAccepting -- new client enqueues are rejected; existing pending requests remain.
\*   3. Draining      -- workers continue picking pending requests off the queue and
\*                       completing them, but no new requests can enter.
\*   4. Cancelling    -- any remaining pending requests AND in-flight requests are
\*                       resolved with a structured ShutdownInProgress error.
\*   5. Joining       -- once every request is in a terminal state (Completed or
\*                       Canceled), workers are joined and the PSR becomes Down.
\*
\* The "missing-drain" bug is modeled by the constant EnableDrain. With EnableDrain = TRUE
\* the spec captures the proposed fix; with EnableDrain = FALSE the Cancelling phase is
\* short-circuited (pending requests are simply abandoned) and the system reaches Down
\* with orphans in `pending` -- the NoOrphanedRequest invariant catches this and TLC
\* prints a counter-example trace.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Requests,    \* Finite set of request identifiers the model considers.
          Workers,     \* Finite set of worker thread identifiers.
          EnableDrain  \* TRUE = proposed fix path. FALSE = bug counter-example path.

\* PSR shutdown phases. Order matters: the protocol monotonically advances through them.
Running       == "Running"
StopAccepting == "StopAccepting"
Draining      == "Draining"
Cancelling    == "Cancelling"
Joining       == "Joining"
Down          == "Down"

Phases == { Running, StopAccepting, Draining, Cancelling, Joining, Down }

\* Per-request terminal statuses.
Completed == "Completed"  \* Worker finished serving the page-read before shutdown drained it.
Canceled  == "Canceled"   \* Resolved with structured ShutdownInProgress error.
Orphaned  == "Orphaned"   \* BUG MARKER: a request was discarded with no client notification.

Terminal == { Completed, Canceled, Orphaned }

VARIABLES phase,        \* Current shutdown phase (one of Phases).
          pending,      \* Sequence of request ids waiting in the queue, in FIFO order.
          inFlight,     \* Function: request id |-> worker id currently serving it.
          done,         \* Function: request id |-> Terminal status.
          workerBusy,   \* Function: worker id |-> BOOLEAN (TRUE = handling a request).
          joined        \* Function: worker id |-> BOOLEAN (TRUE = thread joined).

vars == <<phase, pending, inFlight, done, workerBusy, joined>>

\* Convenience: the set of requests with a known status (terminal).
ResolvedRequests == { r \in Requests : r \in DOMAIN done }

\* Convenience: requests we've seen in flight (assigned to a worker but not yet terminal).
InFlightRequests == DOMAIN inFlight

\* A request is "live" if it is queued or in-flight -- still owed a response.
LiveRequests ==
    { r \in Requests : \E i \in 1..Len(pending) : pending[i] = r } \union InFlightRequests

\* A request has been observed by the PSR at all (queued, in-flight, or terminal).
ObservedRequests == LiveRequests \union ResolvedRequests

\* Find the index of request r in `pending`, or 0 if absent.
PendingIndex(r) ==
    LET indices == { i \in 1..Len(pending) : pending[i] = r }
    IN IF indices = {} THEN 0
       ELSE CHOOSE i \in indices : \A j \in indices : i <= j

\* Remove element at position idx from sequence s.
RemoveAt(s, idx) ==
    SubSeq(s, 1, idx - 1) \o SubSeq(s, idx + 1, Len(s))

\* Helper: build a function with one new key/value pair appended.
WithEntry(f, k, v) == [x \in (DOMAIN f) \union {k} |-> IF x = k THEN v ELSE f[x]]

\* Helper: remove a key from a function's domain.
WithoutKey(f, k) == [x \in (DOMAIN f) \ {k} |-> f[x]]

\* A worker is eligible to pick work iff it's idle and not yet joined.
WorkerIdle(w) == ~workerBusy[w] /\ ~joined[w]

-----------------------------------------------------------------------------
\* Initial state.
-----------------------------------------------------------------------------

Init ==
    /\ phase = Running
    /\ pending = <<>>
    /\ inFlight = <<>>                                  \* Empty function (no domain entries).
    /\ done = <<>>                                      \* Empty function.
    /\ workerBusy = [w \in Workers |-> FALSE]
    /\ joined = [w \in Workers |-> FALSE]

-----------------------------------------------------------------------------
\* Client and worker actions during normal operation.
-----------------------------------------------------------------------------

\* A client submits a brand-new request to the PSR. Only legal while Running.
ClientEnqueue(r) ==
    /\ phase = Running
    /\ r \notin ObservedRequests        \* Each request id is submitted at most once.
    /\ pending' = Append(pending, r)
    /\ UNCHANGED <<phase, inFlight, done, workerBusy, joined>>

\* During StopAccepting or later, a client attempting to submit is rejected with
\* ShutdownInProgress *before* the request ever entered the queue. We model this as a
\* no-op state transition: the request is never added to `pending`, never observed.
\* Captured implicitly (the disjunction in Next does not enable ClientEnqueue once we
\* leave Running) so no separate action is needed.

\* A worker picks up the head of the queue. Legal in Running or Draining.
WorkerPickUp(w) ==
    /\ phase \in { Running, Draining }
    /\ WorkerIdle(w)
    /\ Len(pending) > 0
    /\ LET r == Head(pending) IN
        /\ pending' = Tail(pending)
        /\ inFlight' = WithEntry(inFlight, r, w)
        /\ workerBusy' = [workerBusy EXCEPT ![w] = TRUE]
    /\ UNCHANGED <<phase, done, joined>>

\* A worker finishes serving its in-flight request successfully. Legal in any phase up to
\* and including Cancelling -- a request that's already executing the read can race the
\* cancel signal and win.
WorkerComplete(w) ==
    /\ phase \in { Running, StopAccepting, Draining, Cancelling }
    /\ workerBusy[w]
    /\ \E r \in InFlightRequests :
        /\ inFlight[r] = w
        /\ inFlight' = WithoutKey(inFlight, r)
        /\ done' = WithEntry(done, r, Completed)
        /\ workerBusy' = [workerBusy EXCEPT ![w] = FALSE]
    /\ UNCHANGED <<phase, pending, joined>>

-----------------------------------------------------------------------------
\* Shutdown phase transitions.
-----------------------------------------------------------------------------

\* Step 1: shutdown signal arrives. Stop accepting new clients.
BeginShutdown ==
    /\ phase = Running
    /\ phase' = StopAccepting
    /\ UNCHANGED <<pending, inFlight, done, workerBusy, joined>>

\* Step 2: enter draining. The queue is now closed to new entries; workers continue
\* picking from it until it empties. Only the proposed-fix path takes this step; the
\* buggy path bypasses Draining/Cancelling via BugSkipDrainAndCancel.
EnterDraining ==
    /\ EnableDrain
    /\ phase = StopAccepting
    /\ phase' = Draining
    /\ UNCHANGED <<pending, inFlight, done, workerBusy, joined>>

\* Step 3: enter cancelling. We've stayed in Draining long enough that no further pending
\* requests can be productively picked up under the drain deadline. Move to cancelling.
\* In the proposed fix (EnableDrain = TRUE) the queue may still be non-empty at this
\* boundary; cancellation in Cancelling handles those.
\*
\* In the buggy path (EnableDrain = FALSE) the protocol "skips" both draining and
\* cancelling -- it jumps from StopAccepting straight toward joining (modeled below in
\* BugSkipDrainAndCancel).
EnterCancelling ==
    /\ phase = Draining
    /\ phase' = Cancelling
    /\ UNCHANGED <<pending, inFlight, done, workerBusy, joined>>

\* Step 4 (FIX path): cancel a still-pending request with structured ShutdownInProgress.
CancelPending(r) ==
    /\ EnableDrain
    /\ phase = Cancelling
    /\ PendingIndex(r) > 0
    /\ pending' = RemoveAt(pending, PendingIndex(r))
    /\ done' = WithEntry(done, r, Canceled)
    /\ UNCHANGED <<phase, inFlight, workerBusy, joined>>

\* Step 4 (FIX path): cancel a still-in-flight request whose worker hasn't completed yet.
\* The worker observes the cancel and returns ShutdownInProgress to the client. We
\* model this as the request transitioning to Canceled and the worker becoming idle.
CancelInFlight(r) ==
    /\ EnableDrain
    /\ phase = Cancelling
    /\ r \in InFlightRequests
    /\ LET w == inFlight[r] IN
        /\ inFlight' = WithoutKey(inFlight, r)
        /\ done' = WithEntry(done, r, Canceled)
        /\ workerBusy' = [workerBusy EXCEPT ![w] = FALSE]
    /\ UNCHANGED <<phase, pending, joined>>

\* Step 5 (FIX path): enter joining once everything is terminal.
EnterJoining ==
    /\ phase = Cancelling
    /\ pending = <<>>
    /\ DOMAIN inFlight = {}
    /\ phase' = Joining
    /\ UNCHANGED <<pending, inFlight, done, workerBusy, joined>>

\* Step 6: join an idle worker.
JoinWorker(w) ==
    /\ phase = Joining
    /\ ~workerBusy[w]
    /\ ~joined[w]
    /\ joined' = [joined EXCEPT ![w] = TRUE]
    /\ UNCHANGED <<phase, pending, inFlight, done, workerBusy>>

\* Step 7: once every worker is joined, declare the PSR Down.
DeclareDown ==
    /\ phase = Joining
    /\ \A w \in Workers : joined[w]
    /\ phase' = Down
    /\ UNCHANGED <<pending, inFlight, done, workerBusy, joined>>

-----------------------------------------------------------------------------
\* Bug action: missing-drain shortcut. The current PSR essentially does this -- it leaves
\* Running, declares shutdown complete, and joins workers without ever draining the
\* pending queue or sending structured cancellations.
\*
\* In the model we expose this as a single action that takes the system from
\* StopAccepting directly toward Joining, abandoning everything in `pending` AND
\* everything still in `inFlight`. Abandoned pending requests are stamped Orphaned to
\* make the invariant violation visible; abandoned in-flight requests are also marked
\* Orphaned because their worker thread is forcibly torn down with no structured error
\* delivered to the client.
-----------------------------------------------------------------------------
BugSkipDrainAndCancel ==
    /\ ~EnableDrain
    /\ phase = StopAccepting
    /\ phase' = Joining
    /\ LET orphanedPending == { pending[i] : i \in 1..Len(pending) }
           orphanedFlight  == InFlightRequests
           orphaned        == orphanedPending \union orphanedFlight
       IN  done' = [x \in (DOMAIN done) \union orphaned |->
                       IF x \in orphaned THEN Orphaned ELSE done[x]]
    /\ pending' = <<>>
    /\ inFlight' = <<>>
    /\ workerBusy' = [w \in Workers |-> FALSE]   \* Threads forcibly idled for join.
    /\ UNCHANGED joined

-----------------------------------------------------------------------------
\* Transition relation.
-----------------------------------------------------------------------------

Next ==
    \/ \E r \in Requests : ClientEnqueue(r)
    \/ \E w \in Workers  : WorkerPickUp(w)
    \/ \E w \in Workers  : WorkerComplete(w)
    \/ BeginShutdown
    \/ EnterDraining
    \/ EnterCancelling
    \/ \E r \in Requests : CancelPending(r)
    \/ \E r \in Requests : CancelInFlight(r)
    \/ EnterJoining
    \/ \E w \in Workers  : JoinWorker(w)
    \/ DeclareDown
    \/ BugSkipDrainAndCancel

\* Fairness: once shutdown is signaled, the protocol must make progress. We require
\* weak fairness on every shutdown-advancing action and on the worker actions that
\* drive draining and cancellation forward. Client enqueue is intentionally NOT fair
\* -- clients may stop submitting at any time, which is realistic.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(BeginShutdown)
    /\ WF_vars(EnterDraining)
    /\ WF_vars(EnterCancelling)
    /\ WF_vars(EnterJoining)
    /\ WF_vars(DeclareDown)
    /\ WF_vars(BugSkipDrainAndCancel)
    /\ \A w \in Workers :
        /\ WF_vars(WorkerPickUp(w))
        /\ WF_vars(WorkerComplete(w))
        /\ WF_vars(JoinWorker(w))
    /\ \A r \in Requests :
        /\ WF_vars(CancelPending(r))
        /\ WF_vars(CancelInFlight(r))

-----------------------------------------------------------------------------
\* Type invariant.
-----------------------------------------------------------------------------

TypeOK ==
    /\ phase \in Phases
    /\ \A i \in 1..Len(pending) : pending[i] \in Requests
    /\ DOMAIN inFlight \subseteq Requests
    /\ \A r \in DOMAIN inFlight : inFlight[r] \in Workers
    /\ DOMAIN done \subseteq Requests
    /\ \A r \in DOMAIN done : done[r] \in Terminal
    /\ workerBusy \in [Workers -> BOOLEAN]
    /\ joined \in [Workers -> BOOLEAN]

-----------------------------------------------------------------------------
\* Safety invariants.
-----------------------------------------------------------------------------

\* (Bookkeeping) Each request is in at most one of {pending, in-flight, done}.
RequestStatesDisjoint ==
    \A r \in Requests :
        LET inPending == \E i \in 1..Len(pending) : pending[i] = r
            inFly     == r \in DOMAIN inFlight
            inDone    == r \in DOMAIN done
        IN  (inPending => ~inFly /\ ~inDone)
            /\ (inFly => ~inPending /\ ~inDone)
            /\ (inDone => ~inPending /\ ~inFly)

\* Worker busy bit agrees with the inFlight map.
WorkerBookkeepingOK ==
    \A w \in Workers :
        workerBusy[w] <=> (\E r \in DOMAIN inFlight : inFlight[r] = w)

\* Each worker appears at most once in inFlight.
WorkerUniqueAssignment ==
    \A r1, r2 \in DOMAIN inFlight :
        (r1 # r2) => inFlight[r1] # inFlight[r2]

\* The headline invariant for SERVER-113702: no observed request is ever orphaned.
\* When the protocol drains and cancels correctly, every observed request ends up
\* Completed or Canceled -- never Orphaned. With EnableDrain = FALSE the bug action
\* writes Orphaned into `done` and TLC immediately surfaces the trace.
NoOrphanedRequest ==
    \A r \in DOMAIN done : done[r] # Orphaned

\* Companion invariant: once Down, every request the PSR observed has a terminal,
\* non-orphan status. Catches the bug from the "after-the-fact" angle in case the
\* Orphaned tag is masked.
ShutdownCompletesCleanly ==
    (phase = Down) =>
        /\ pending = <<>>
        /\ DOMAIN inFlight = {}
        /\ \A r \in ObservedRequests :
            /\ r \in DOMAIN done
            /\ done[r] \in { Completed, Canceled }

-----------------------------------------------------------------------------
\* Liveness properties.
-----------------------------------------------------------------------------

\* Every observed request eventually reaches a non-orphan terminal status.
EveryRequestResolved ==
    \A r \in Requests :
        (r \in ObservedRequests) ~>
            (r \in DOMAIN done /\ done[r] \in { Completed, Canceled })

\* Shutdown, once signaled, eventually completes.
ShutdownTerminates ==
    (phase # Running) ~> (phase = Down)

\* Every worker is eventually joined.
AllWorkersEventuallyJoined ==
    \A w \in Workers :
        (phase # Running) ~> joined[w]

=================================================================================================
