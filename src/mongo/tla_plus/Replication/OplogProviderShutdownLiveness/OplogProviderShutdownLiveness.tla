\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------ MODULE OplogProviderShutdownLiveness ------------------------
\* Models OplogProvider::stop() and a pool of consumer tasks scheduled on the
\* provider's scoped executor. Captures the deadlock reported in SERVER-125455:
\* the stop path performs
\*
\*   scopedExecutor->shutdown();
\*   scopedExecutor->join();          // <-- waits forever
\*   _state = nullptr;
\*   _opCtxHolder = {};               // <-- destructed too late
\*
\* If a consumer task is blocked in an uninterruptible resource wait (e.g. WT
\* eviction inside WiredTigerRecoveryUnit::_txnOpen), the only way to unblock
\* it is to mark its OperationContext killed. The bug ordering kills the
\* OperationContext only after the join returns -- which it never does -- so
\* mongod hangs forever.
\*
\* The model has two switches:
\*   * KillOpCtxBeforeJoin   = TRUE  -- the fix; stop() marks every consumer's
\*                                      opCtx killed before waiting on join().
\*                            FALSE -- the buggy ordering shipped today.
\*   * InterruptibleResource = TRUE  -- the resource wait already polls
\*                                      opCtx->checkForInterrupt(); models a
\*                                      world without the eviction trap.
\*                            FALSE -- the wait is uninterruptible until the
\*                                      consumer's opCtx is killed; matches
\*                                      the WT-eviction trap from the ticket.
\*
\* The two BAIT configs (MCOplogProviderShutdownLiveness_bug.cfg) flip these
\* to the worst-case combination and let TLC produce the counterexample
\* trace that hangs mongod.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Consumers,             \* Set of consumer task identities.
          KillOpCtxBeforeJoin,   \* TRUE = fix ordering; FALSE = current ordering.
          InterruptibleResource  \* TRUE = resource wait honours opCtx; FALSE = WT trap.

\* Provider thread (the one that runs OplogProvider::stop()).
ProviderThread == "Provider"

\* Provider state machine.
\*   "idle"          steady state, consumers free to run.
\*   "signalled"     stop() called, executor->shutdown() done; no new tasks accepted.
\*   "joining"       blocked in executor->join() until every consumer is done.
\*   "exited"        join returned and OplogProvider thread has exited.
ProviderStates == {"idle", "signalled", "joining", "exited"}

\* Consumer task state machine.
\*   "running"       executing on the scoped executor, not currently blocked.
\*   "blocked"       parked in a resource wait (e.g. WT eviction); only an
\*                   interrupt or the resource freeing unblocks it.
\*   "interrupted"   opCtx has been marked killed; consumer will unwind.
\*   "done"          task finished and released its executor slot.
ConsumerStates == {"running", "blocked", "interrupted", "done"}

VARIABLES
    providerState,   \* element of ProviderStates.
    consumerState,   \* [Consumers -> ConsumerStates].
    opCtxKilled,     \* [Consumers -> BOOLEAN]; TRUE once opCtx marked killed.
    resourceFree     \* BOOLEAN; TRUE once the resource the consumer is parked on releases.

vars == <<providerState, consumerState, opCtxKilled, resourceFree>>

-----------------------------------------------------------------------------

AllDone == \A c \in Consumers : consumerState[c] = "done"

AnyBlocked == \E c \in Consumers : consumerState[c] = "blocked"

\* In the buggy ordering, opCtxs are destructed (and therefore can't be killed
\* in time) until *after* join() returns. We model that by gating the opCtx
\* kill action on either the fix being enabled, or the provider having reached
\* "exited" -- which it never does while a consumer is still blocked, so the
\* condition reduces to "fix enabled" for the deadlock scenarios we care about.
CanKillOpCtx ==
    \/ KillOpCtxBeforeJoin
    \/ providerState = "exited"

-----------------------------------------------------------------------------

Init ==
    /\ providerState = "idle"
    /\ consumerState = [c \in Consumers |-> "running"]
    /\ opCtxKilled = [c \in Consumers |-> FALSE]
    /\ resourceFree = FALSE

\* A running consumer parks on an external resource (models WT eviction etc.).
ConsumerBlocksOnResource(c) ==
    /\ consumerState[c] = "running"
    /\ consumerState' = [consumerState EXCEPT ![c] = "blocked"]
    /\ UNCHANGED <<providerState, opCtxKilled, resourceFree>>

\* The external resource finishes on its own (the eviction front clears).
ResourceReleases ==
    /\ ~resourceFree
    /\ resourceFree' = TRUE
    /\ UNCHANGED <<providerState, consumerState, opCtxKilled>>

\* A blocked consumer wakes because the resource finally released. Only valid
\* when the wait is interruptible-by-resource OR the resource has freed.
ConsumerWakesOnResource(c) ==
    /\ consumerState[c] = "blocked"
    /\ resourceFree
    /\ consumerState' = [consumerState EXCEPT ![c] = "running"]
    /\ UNCHANGED <<providerState, opCtxKilled, resourceFree>>

\* The provider thread observes a shutdown request and calls
\* scopedExecutor->shutdown(). No new tasks are accepted after this; live
\* tasks keep running until they reach a yield point.
SignalShutdown ==
    /\ providerState = "idle"
    /\ providerState' = "signalled"
    /\ UNCHANGED <<consumerState, opCtxKilled, resourceFree>>

\* The fix: mark every consumer's opCtx killed before entering join().
\* Idempotent; running this once is enough but the spec allows re-firing.
KillAllConsumerOpCtxs ==
    /\ providerState = "signalled"
    /\ KillOpCtxBeforeJoin
    /\ opCtxKilled' = [c \in Consumers |-> TRUE]
    /\ UNCHANGED <<providerState, consumerState, resourceFree>>

\* Provider enters executor->join(); it will not progress until every
\* consumer is in "done". With the fix ordering, the provider is required
\* to mark every consumer's opCtx killed *before* entering join() -- that's
\* the whole point of the patch. Without the fix, EnterJoin is free to fire
\* immediately after SignalShutdown, modelling today's broken ordering.
EnterJoin ==
    /\ providerState = "signalled"
    /\ KillOpCtxBeforeJoin => \A c \in Consumers : opCtxKilled[c]
    /\ providerState' = "joining"
    /\ UNCHANGED <<consumerState, opCtxKilled, resourceFree>>

\* A consumer notices its opCtx has been killed and unwinds out of the
\* resource wait. If the wait is uninterruptible-by-design (WT trap),
\* the opCtx kill is the only thing that can unblock it.
ConsumerObservesKill(c) ==
    /\ consumerState[c] \in {"running", "blocked"}
    /\ opCtxKilled[c]
    /\ consumerState' = [consumerState EXCEPT ![c] = "interrupted"]
    /\ UNCHANGED <<providerState, opCtxKilled, resourceFree>>

\* An interrupted (or running, if it cooperatively yielded) consumer finishes
\* its task and releases its executor slot.
ConsumerFinishes(c) ==
    /\ consumerState[c] \in {"running", "interrupted"}
    /\ \/ consumerState[c] = "interrupted"
       \/ providerState \in {"signalled", "joining"}
    /\ consumerState' = [consumerState EXCEPT ![c] = "done"]
    /\ UNCHANGED <<providerState, opCtxKilled, resourceFree>>

\* Optional: a blocked consumer whose resource wait is interruptible can self
\* unblock once the resource releases without an opCtx kill. Models the
\* "no WT trap" branch.
ConsumerSelfUnblocks(c) ==
    /\ InterruptibleResource
    /\ consumerState[c] = "blocked"
    /\ resourceFree
    /\ consumerState' = [consumerState EXCEPT ![c] = "running"]
    /\ UNCHANGED <<providerState, opCtxKilled, resourceFree>>

\* join() returns once every consumer is done; the OplogProvider thread exits.
JoinReturnsAndExit ==
    /\ providerState = "joining"
    /\ AllDone
    /\ providerState' = "exited"
    /\ UNCHANGED <<consumerState, opCtxKilled, resourceFree>>

\* After exit the buggy ordering destructs _opCtxHolder. We model that by
\* allowing opCtx kill to fire post-exit -- it is harmless because every
\* consumer is already "done" -- and it preserves the (TLA) invariant that
\* opCtxKilled is monotone.
LateOpCtxKill ==
    /\ providerState = "exited"
    /\ ~KillOpCtxBeforeJoin
    /\ opCtxKilled' = [c \in Consumers |-> TRUE]
    /\ UNCHANGED <<providerState, consumerState, resourceFree>>

-----------------------------------------------------------------------------

Next ==
    \/ \E c \in Consumers : ConsumerBlocksOnResource(c)
    \/ ResourceReleases
    \/ \E c \in Consumers : ConsumerWakesOnResource(c)
    \/ SignalShutdown
    \/ KillAllConsumerOpCtxs
    \/ EnterJoin
    \/ \E c \in Consumers : ConsumerObservesKill(c)
    \/ \E c \in Consumers : ConsumerFinishes(c)
    \/ \E c \in Consumers : ConsumerSelfUnblocks(c)
    \/ JoinReturnsAndExit
    \/ LateOpCtxKill

\* Fairness:
\*  - The provider thread itself is fair (must eventually signal, kill, join, exit).
\*  - Every consumer must eventually finish if it can.
\*  - The resource is *not* assumed to release on its own; that's the whole point
\*    of the WT-eviction trap. Liveness has to come from the opCtx-kill path.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(SignalShutdown)
    /\ WF_vars(KillAllConsumerOpCtxs)
    /\ WF_vars(EnterJoin)
    /\ WF_vars(JoinReturnsAndExit)
    /\ \A c \in Consumers : /\ WF_vars(ConsumerObservesKill(c))
                            /\ WF_vars(ConsumerFinishes(c))

-----------------------------------------------------------------------------
\* Invariants
-----------------------------------------------------------------------------

TypeOK ==
    /\ providerState \in ProviderStates
    /\ consumerState \in [Consumers -> ConsumerStates]
    /\ opCtxKilled \in [Consumers -> BOOLEAN]
    /\ resourceFree \in BOOLEAN

\* The provider must not advance to "exited" while any consumer task is still
\* live -- that would be an executor bug (join returned with tasks running).
NoExitWhileBusy ==
    providerState = "exited" => AllDone

\* Buggy ordering invariant: in the broken world we never kill opCtxs before
\* join. This is a sanity guard, not a correctness property.
KillOrderingHonored ==
    (~KillOpCtxBeforeJoin /\ providerState \in {"idle", "signalled", "joining"})
        => \A c \in Consumers : ~opCtxKilled[c]

-----------------------------------------------------------------------------
\* Liveness
-----------------------------------------------------------------------------

\* Headline property: a stop signal eventually unwinds every consumer and the
\* OplogProvider thread reaches "exited". This is what SERVER-125455 violates.
ShutdownEventuallyExits ==
    (providerState = "signalled") ~> (providerState = "exited")

\* Every consumer eventually reaches "done" once shutdown is signalled.
ConsumersEventuallyDone ==
    \A c \in Consumers :
        (providerState = "signalled") ~> (consumerState[c] = "done")

\* If we ever enter join(), we eventually leave it.
JoinEventuallyReturns ==
    (providerState = "joining") ~> (providerState = "exited")
=====================================================================================
