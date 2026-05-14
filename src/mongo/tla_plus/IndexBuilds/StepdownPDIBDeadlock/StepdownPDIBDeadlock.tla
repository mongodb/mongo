\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE StepdownPDIBDeadlock -------------------------
\* Formal specification of the SERVER-126266 deadlock between the stepdown
\* coordinator thread and the primary-driven index build (PDIB) coordinator.
\*
\* The bug:  on a steady-state primary running a PDIB, a stepdown is issued.
\* Stepdown enqueues an X-mode waiter on the Replication State Transition
\* Lock (RSTL).  Once the X-waiter is enqueued, BackgroundSync, which needs
\* to apply incoming oplog entries (including PDIB votes from other nodes)
\* under an IX intent on the RSTL, blocks behind the waiter.  Meanwhile the
\* PDIB coordinator is awaiting commit-quorum votes that will arrive only
\* through BackgroundSync, while continuing to hold its own IX intent on the
\* RSTL.  The wait-for graph is:
\*
\*     stepdown(X)  --waits-for-->  pdib(IX)         (RSTL queue)
\*     pdib(IX)     --waits-for-->  bgsync(IX-vote)  (commit quorum)
\*     bgsync(IX)   --waits-for-->  stepdown(X)      (RSTL queue, fair)
\*
\* The cycle is closed.  Stepdown's wait-loop never sees the secondaries
\* catch up; on each iteration tryToStepDown() fails, the RSTL X waiter is
\* re-enqueued, and the cycle restarts.
\*
\* The model has three actors (Stepdown, PDIB, BgSync) racing on one RSTL
\* and one shared PDIB state machine.  We check (a) no cycle ever appears in
\* the wait-for graph (DeadlockFreedom) and (b) every initiated stepdown
\* eventually completes (EveryStepdownCompletes).  The bug toggle
\* AllowPDIBHoldDuringStepdown selects the buggy vs the fixed behaviour:
\* when TRUE, the PDIB coordinator may keep its RSTL IX intent across an
\* enqueued stepdown X waiter, which is what the production code does today.
\* When FALSE, PDIB observes the stepdown signal and drops its IX intent
\* (the patch direction).
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh IndexBuilds/StepdownPDIBDeadlock

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The PDIB coordinator's distinguished thread id.
CONSTANT PDIB
\* The stepdown coordinator's distinguished thread id.
CONSTANT Stepdown
\* The BackgroundSync thread id.
CONSTANT BgSync

\* The set of thread ids contending on the RSTL.
Threads == {PDIB, Stepdown, BgSync}

\* Bug toggle.  TRUE = production behaviour (deadlock-prone).
\* FALSE = patched behaviour (PDIB releases IX on stepdown signal).
CONSTANT AllowPDIBHoldDuringStepdown

\* Number of secondaries whose votes are required for commit-quorum.  Two
\* in MaxQuorum suffices to surface the bug; bumping it just grows the
\* state space without changing reachability.
CONSTANT MaxQuorum

----
\* PDIB coordinator phase.  See src/mongo/db/index_builds/primary_driven/.
\*  - "idle"       : no PDIB in flight.
\*  - "scanning"   : collection scan; IX RSTL held.
\*  - "voting"     : awaiting commit-quorum votes; IX RSTL held.
\*  - "committing" : commitIndexBuild written; IX RSTL held.
\*  - "aborted"    : PDIB aborted; IX RSTL released.
\*  - "done"       : committed; IX RSTL released.
VARIABLE pdibPhase

\* The set of secondaries that have voted in the commit quorum.  Models
\* the OpObserver path on the primary observing replicated votes.
VARIABLE commitQuorumVotes

\* RSTL holder.  Holders is a function thread -> {"none","IX","X"}.
VARIABLE rstlMode

\* The FIFO RSTL waiter queue.  Sequence of <<thread, requestedMode>>.
\* MongoDB's RSTL is approximately fair: an X waiter parks IX requesters
\* enqueued behind it (see auto_get_rstl_for_stepup_stepdown.h).
VARIABLE rstlQueue

\* Stepdown control: phases per stepdown call.
\*  - "none"     : no stepdown in flight.
\*  - "enqueued" : X waiter on RSTL, awaiting acquisition.
\*  - "holding"  : X acquired; running post-acquisition logic.
\*  - "completed": stepDown() returned.
VARIABLE stepdownPhase

\* Whether PDIB has observed the stepdown signal (the fixed code path
\* asynchronously cancels PDIB on a stepdown-initiated callback).
VARIABLE pdibObservedStepdown

vars == << pdibPhase, commitQuorumVotes, rstlMode, rstlQueue,
           stepdownPhase, pdibObservedStepdown >>

----
\* Helpers.

\* Set of threads holding the RSTL in any mode.
Holders == { t \in Threads : rstlMode[t] \in {"IX","X"} }

\* Does any thread hold the RSTL in X mode?
XHeld == \E t \in Threads : rstlMode[t] = "X"

\* The set of threads parked behind an X waiter.  This is the "fair queue"
\* behaviour: once an X is enqueued, no further IX may be granted ahead of
\* it.  Returns TRUE if there is at least one X already enqueued or held.
XPendingOrHeld ==
    \/ XHeld
    \/ \E i \in 1..Len(rstlQueue) : rstlQueue[i][2] = "X"

\* "thread t holds RSTL in mode m" predicate
HoldsRSTL(t, m) == rstlMode[t] = m

\* The first waiter in the queue.
QueueHead == IF Len(rstlQueue) > 0 THEN rstlQueue[1] ELSE <<"none","none">>

\* IsMajority over MaxQuorum secondaries  (primary's own vote is implicit).
\* The primary's vote is added in CommitQuorumReached so this just counts
\* observed secondary votes plus the primary's implicit one.
IsMajority(votes) == (Cardinality(votes) + 1) * 2 > (MaxQuorum + 1)

----
\* Wait-for graph.  Edge (t1, t2) means "t1 is waiting for t2 to release
\* some resource".  Two edge sources:
\*   1. RSTL queue: t1 enqueued, t2 holds an incompatible mode.
\*   2. CommitQuorum: PDIB in "voting" waits for BgSync to apply votes if
\*      BgSync is itself parked on the RSTL.
WaitsForRSTL(t1, t2) ==
    /\ t1 # t2
    /\ \E i \in 1..Len(rstlQueue) : rstlQueue[i][1] = t1
    /\ rstlMode[t2] \in {"IX","X"}
    /\ \* incompatibility
       \/ rstlMode[t2] = "X"
       \/ /\ rstlMode[t2] = "IX"
          /\ \E j \in 1..Len(rstlQueue) :
                 /\ rstlQueue[j][1] = t1
                 /\ rstlQueue[j][2] = "X"

WaitsForQuorum(t1, t2) ==
    /\ t1 = PDIB
    /\ t2 = BgSync
    /\ pdibPhase = "voting"
    /\ ~IsMajority(commitQuorumVotes)
    /\ \* BgSync cannot apply oplog entries because it's parked on the RSTL.
       \E i \in 1..Len(rstlQueue) : rstlQueue[i][1] = BgSync

WaitsFor(t1, t2) == WaitsForRSTL(t1, t2) \/ WaitsForQuorum(t1, t2)

\* TLA+ has no built-in transitive closure.  For three threads we can
\* enumerate cycles of length 2 and 3 directly.
HasCycle ==
    \/ \E t1, t2 \in Threads :
           /\ t1 # t2
           /\ WaitsFor(t1, t2)
           /\ WaitsFor(t2, t1)
    \/ \E t1, t2, t3 \in Threads :
           /\ t1 # t2 /\ t2 # t3 /\ t1 # t3
           /\ WaitsFor(t1, t2)
           /\ WaitsFor(t2, t3)
           /\ WaitsFor(t3, t1)

----
\* Initial state.

Init ==
    /\ pdibPhase = "idle"
    /\ commitQuorumVotes = {}
    /\ rstlMode = [ t \in Threads |-> "none" ]
    /\ rstlQueue = << >>
    /\ stepdownPhase = "none"
    /\ pdibObservedStepdown = FALSE

----
\* Actions.

\* PDIB coordinator starts an index build.  Acquires IX RSTL if no X is
\* pending.  If an X waiter is already enqueued, PDIB queues behind it.
PDIBStart ==
    /\ pdibPhase = "idle"
    /\ stepdownPhase \in {"none","enqueued"}
    /\ IF XPendingOrHeld
         THEN /\ rstlQueue' = Append(rstlQueue, <<PDIB, "IX">>)
              /\ UNCHANGED rstlMode
         ELSE /\ rstlMode' = [rstlMode EXCEPT ![PDIB] = "IX"]
              /\ UNCHANGED rstlQueue
    /\ pdibPhase' = "scanning"
    /\ UNCHANGED << commitQuorumVotes, stepdownPhase, pdibObservedStepdown >>

PDIBFinishScan ==
    /\ pdibPhase = "scanning"
    /\ HoldsRSTL(PDIB, "IX")
    /\ pdibPhase' = "voting"
    /\ UNCHANGED << commitQuorumVotes, rstlMode, rstlQueue,
                    stepdownPhase, pdibObservedStepdown >>

\* A secondary's vote becomes observable on the primary, but ONLY if
\* BgSync has been able to apply the entry.  We model BgSync's progress
\* by gating vote arrival on BgSync's RSTL state: BgSync must hold IX
\* (or no thread holds X and the queue head is BgSync) to apply.
PDIBReceiveVote(s) ==
    /\ pdibPhase = "voting"
    /\ s \notin commitQuorumVotes
    /\ Cardinality(commitQuorumVotes) < MaxQuorum
    /\ \* BgSync can apply oplog entries.
       \/ HoldsRSTL(BgSync, "IX")
       \/ /\ ~XHeld
          /\ rstlQueue = << >>
    /\ commitQuorumVotes' = commitQuorumVotes \cup {s}
    /\ UNCHANGED << pdibPhase, rstlMode, rstlQueue,
                    stepdownPhase, pdibObservedStepdown >>

\* PDIB sees commit quorum.  Transitions voting -> committing.
PDIBCommitQuorumReached ==
    /\ pdibPhase = "voting"
    /\ IsMajority(commitQuorumVotes)
    /\ HoldsRSTL(PDIB, "IX")
    /\ pdibPhase' = "committing"
    /\ UNCHANGED << commitQuorumVotes, rstlMode, rstlQueue,
                    stepdownPhase, pdibObservedStepdown >>

PDIBFinishCommit ==
    /\ pdibPhase = "committing"
    /\ HoldsRSTL(PDIB, "IX")
    /\ rstlMode' = [rstlMode EXCEPT ![PDIB] = "none"]
    /\ pdibPhase' = "done"
    /\ UNCHANGED << commitQuorumVotes, rstlQueue,
                    stepdownPhase, pdibObservedStepdown >>

\* PDIB abort path.  On the patched code (AllowPDIBHoldDuringStepdown =
\* FALSE), the stepdown callback marks pdibObservedStepdown = TRUE and the
\* coordinator must release IX and transition to "aborted".  On the buggy
\* path this action is disabled while in "voting" --- mirroring the
\* observed live behaviour.
PDIBAbortOnStepdown ==
    /\ ~AllowPDIBHoldDuringStepdown
    /\ pdibPhase \in {"scanning","voting","committing"}
    /\ pdibObservedStepdown
    /\ HoldsRSTL(PDIB, "IX")
    /\ rstlMode' = [rstlMode EXCEPT ![PDIB] = "none"]
    /\ pdibPhase' = "aborted"
    /\ UNCHANGED << commitQuorumVotes, rstlQueue,
                    stepdownPhase, pdibObservedStepdown >>

\* Stepdown begins.  Enqueues X on RSTL.
StepdownStart ==
    /\ stepdownPhase = "none"
    /\ ~XHeld
    /\ stepdownPhase' = "enqueued"
    /\ rstlQueue' = Append(rstlQueue, <<Stepdown, "X">>)
    /\ pdibObservedStepdown' = ~AllowPDIBHoldDuringStepdown
    /\ UNCHANGED << pdibPhase, commitQuorumVotes, rstlMode >>

\* Stepdown promotes from queue to holder once no IX is held.  In the
\* production code, the X waiter blocks until all IX holders drain.
StepdownAcquireX ==
    /\ stepdownPhase = "enqueued"
    /\ Len(rstlQueue) > 0
    /\ rstlQueue[1] = <<Stepdown, "X">>
    /\ \A t \in Threads : rstlMode[t] = "none"
    /\ rstlMode' = [rstlMode EXCEPT ![Stepdown] = "X"]
    /\ rstlQueue' = Tail(rstlQueue)
    /\ stepdownPhase' = "holding"
    /\ UNCHANGED << pdibPhase, commitQuorumVotes, pdibObservedStepdown >>

\* Stepdown completes and releases X.
StepdownComplete ==
    /\ stepdownPhase = "holding"
    /\ HoldsRSTL(Stepdown, "X")
    /\ rstlMode' = [rstlMode EXCEPT ![Stepdown] = "none"]
    /\ stepdownPhase' = "completed"
    /\ UNCHANGED << pdibPhase, commitQuorumVotes, rstlQueue,
                    pdibObservedStepdown >>

\* BgSync acquires IX so it can apply oplog entries (the vote-carrying
\* entries the PDIB coordinator is awaiting).  Blocked by any pending X.
BgSyncAcquire ==
    /\ ~HoldsRSTL(BgSync, "IX")
    /\ ~XPendingOrHeld
    /\ rstlMode' = [rstlMode EXCEPT ![BgSync] = "IX"]
    /\ UNCHANGED << pdibPhase, commitQuorumVotes, rstlQueue,
                    stepdownPhase, pdibObservedStepdown >>

\* BgSync queues if an X is pending.
BgSyncEnqueue ==
    /\ ~HoldsRSTL(BgSync, "IX")
    /\ XPendingOrHeld
    /\ ~ \E i \in 1..Len(rstlQueue) : rstlQueue[i][1] = BgSync
    /\ rstlQueue' = Append(rstlQueue, <<BgSync, "IX">>)
    /\ UNCHANGED << pdibPhase, commitQuorumVotes, rstlMode,
                    stepdownPhase, pdibObservedStepdown >>

\* BgSync drains from queue once X holder releases and queue head is its
\* request (or no X is pending).  Approximates the lock manager's grant
\* loop on RSTL release.
BgSyncGrant ==
    /\ \E i \in 1..Len(rstlQueue) : rstlQueue[i][1] = BgSync
    /\ ~XHeld
    /\ \* No earlier X waiter ahead of BgSync.
       LET myIdx == CHOOSE i \in 1..Len(rstlQueue) : rstlQueue[i][1] = BgSync
        IN \A j \in 1..(myIdx - 1) : rstlQueue[j][2] # "X"
    /\ LET myIdx == CHOOSE i \in 1..Len(rstlQueue) : rstlQueue[i][1] = BgSync
        IN rstlQueue' =
             [ k \in 1..(Len(rstlQueue) - 1) |->
                  IF k < myIdx THEN rstlQueue[k] ELSE rstlQueue[k+1] ]
    /\ rstlMode' = [rstlMode EXCEPT ![BgSync] = "IX"]
    /\ UNCHANGED << pdibPhase, commitQuorumVotes,
                    stepdownPhase, pdibObservedStepdown >>

\* BgSync releases IX (for example when it goes idle between batches).
BgSyncRelease ==
    /\ HoldsRSTL(BgSync, "IX")
    /\ rstlMode' = [rstlMode EXCEPT ![BgSync] = "none"]
    /\ UNCHANGED << pdibPhase, commitQuorumVotes, rstlQueue,
                    stepdownPhase, pdibObservedStepdown >>

----
\* Next state relation.

Next ==
    \/ PDIBStart
    \/ PDIBFinishScan
    \/ \E s \in 1..MaxQuorum : PDIBReceiveVote(s)
    \/ PDIBCommitQuorumReached
    \/ PDIBFinishCommit
    \/ PDIBAbortOnStepdown
    \/ StepdownStart
    \/ StepdownAcquireX
    \/ StepdownComplete
    \/ BgSyncAcquire
    \/ BgSyncEnqueue
    \/ BgSyncGrant
    \/ BgSyncRelease

Spec == Init /\ [][Next]_vars

\* Fairness for the liveness check.  Without these, TLC could let
\* StepdownAcquireX never fire even though it's enabled.  BgSyncRelease
\* gets strong fairness so the modelled background sync can yield its IX
\* intent between oplog batches when a stepdown X waiter is parked behind
\* it, which is how production BgSync behaves.
Liveness ==
    /\ WF_vars(StepdownAcquireX)
    /\ WF_vars(StepdownComplete)
    /\ WF_vars(BgSyncGrant)
    /\ WF_vars(BgSyncAcquire)
    /\ SF_vars(BgSyncRelease)
    /\ WF_vars(PDIBAbortOnStepdown)
    /\ WF_vars(PDIBFinishCommit)
    /\ \A s \in 1..MaxQuorum : WF_vars(PDIBReceiveVote(s))

FairSpec == Spec /\ Liveness

----
\* Invariants and properties.

\* SAFETY: the wait-for graph never closes a cycle.
DeadlockFreedom == ~HasCycle

\* SAFETY: RSTL is held in X by at most one thread, and X is incompatible
\* with any IX hold.
RSTLWellFormed ==
    /\ Cardinality({ t \in Threads : rstlMode[t] = "X" }) <= 1
    /\ \/ ~XHeld
       \/ \A t \in Threads : rstlMode[t] # "IX"

\* TYPE invariant.
TypeOK ==
    /\ pdibPhase \in {"idle","scanning","voting","committing","aborted","done"}
    /\ commitQuorumVotes \subseteq 1..MaxQuorum
    /\ rstlMode \in [Threads -> {"none","IX","X"}]
    /\ stepdownPhase \in {"none","enqueued","holding","completed"}
    /\ pdibObservedStepdown \in BOOLEAN

\* LIVENESS: every issued stepdown eventually completes.
EveryStepdownCompletes ==
    [](stepdownPhase = "enqueued" ~> stepdownPhase = "completed")

==============================================================================
