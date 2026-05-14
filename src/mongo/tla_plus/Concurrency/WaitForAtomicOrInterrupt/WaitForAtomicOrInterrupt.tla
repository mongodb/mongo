\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE WaitForAtomicOrInterrupt ---------------------------------------
\* Models OperationContext::waitForAtomicOrInterrupt (SERVER-75430).
\*
\* A wait function joins two wake sources: (a) an atomic flag flipped by a producer (e.g. a ticket
\* release in TicketPool / PriorityTicketHolder) and (b) the interrupt mechanism on OperationContext
\* (killOp, shutdown, deadline). Today, futex waiters in TicketPool sleep on the atomic only and
\* poll for interrupt every 500ms. That 500ms poll is the bug: under load, every operation queues
\* > 500ms, times out, re-queues, and the system enters a metastable failure state where shutdown
\* and killOp can stall for seconds-to-minutes.
\*
\* This spec models the interruptible variant. The thread joins both wake sources atomically: a
\* wake from the atomic OR a kill bit flip exits the wait. The bug config replaces the joined wait
\* with a bare atomic wait (no interrupt branch) and demonstrates the resulting liveness failure.
\*
\* Spec at a glance:
\*   - Threads = set of waiter threads.
\*   - State per thread \in {Running, Waiting, Done}.
\*   - Atomic flag flips False -> True at most once per "generation"; flipping wakes one waiter.
\*   - Interrupt flag flips False -> True at most once per thread.
\*   - Either source returning unblocks the wait.
\*   - Liveness: from any state in which Interrupt(t) has fired, t eventually leaves Waiting.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Threads,                  \* Set of waiter threads.
          MaxAtomicFlips,           \* Bound on producer-side flips (state-space cap).
          InterruptIsResponsive     \* Boolean: when FALSE, the wait does NOT check interrupt
                                    \* (bug config; liveness counter-example).

ASSUME MaxAtomicFlips \in Nat
ASSUME InterruptIsResponsive \in BOOLEAN

VARIABLES
    state,         \* state[t] \in {"Running", "Waiting", "Done"}
    atomic,        \* Boolean: shared atomic flag (TRUE => ticket/permit available).
    interrupted,   \* interrupted[t] \in BOOLEAN: per-thread kill bit.
    atomicFlips,   \* Nat: number of times atomic transitioned False -> True (bounded).
    exitReason     \* exitReason[t] \in {"none", "atomic", "interrupt"}

vars == <<state, atomic, interrupted, atomicFlips, exitReason>>

States == {"Running", "Waiting", "Done"}
ExitReasons == {"none", "atomic", "interrupt"}

-----------------------------------------------------------------------------

\* True iff at least one Waiting thread observes the wake condition.
SomeWaiterCanWake ==
    \E t \in Threads :
        /\ state[t] = "Waiting"
        /\ \/ atomic                                             \* atomic source
           \/ (InterruptIsResponsive /\ interrupted[t])           \* interrupt source

-----------------------------------------------------------------------------

Init ==
    /\ state        = [t \in Threads |-> "Running"]
    /\ atomic       = FALSE
    /\ interrupted  = [t \in Threads |-> FALSE]
    /\ atomicFlips  = 0
    /\ exitReason   = [t \in Threads |-> "none"]

\* A running thread enters the wait. It will not progress further until either:
\*   1. atomic is set (and it consumes the wake), or
\*   2. its interrupt bit is set AND the variant under test checks it.
EnterWait(t) ==
    /\ state[t] = "Running"
    /\ state' = [state EXCEPT ![t] = "Waiting"]
    /\ UNCHANGED <<atomic, interrupted, atomicFlips, exitReason>>

\* Producer flips the atomic to TRUE (bounded by MaxAtomicFlips). Real implementations use $merge-
\* like idempotent CAS; the abstraction is: "wake source A becomes live".
FlipAtomic ==
    /\ ~atomic
    /\ atomicFlips < MaxAtomicFlips
    /\ atomic'      = TRUE
    /\ atomicFlips' = atomicFlips + 1
    /\ UNCHANGED <<state, interrupted, exitReason>>

\* An external thread (killOp / shutdown) flips a waiter's interrupt bit.
Interrupt(t) ==
    /\ ~interrupted[t]
    /\ interrupted' = [interrupted EXCEPT ![t] = TRUE]
    /\ UNCHANGED <<state, atomic, atomicFlips, exitReason>>

\* A waiting thread is woken by the atomic. It consumes the wake (resets the flag) and exits.
WakeOnAtomic(t) ==
    /\ state[t] = "Waiting"
    /\ atomic
    /\ atomic'     = FALSE
    /\ state'      = [state      EXCEPT ![t] = "Done"]
    /\ exitReason' = [exitReason EXCEPT ![t] = "atomic"]
    /\ UNCHANGED <<interrupted, atomicFlips>>

\* A waiting thread is woken by the interrupt bit. Only enabled when the variant under test
\* couples the interrupt path to the wait. With InterruptIsResponsive = FALSE this disjunct
\* is structurally disabled, modelling a bare atomic wait that polls (or never polls) for
\* interrupt and reproducing the SERVER-75430 metastable-failure pre-condition.
WakeOnInterrupt(t) ==
    /\ InterruptIsResponsive
    /\ state[t] = "Waiting"
    /\ interrupted[t]
    /\ state'      = [state      EXCEPT ![t] = "Done"]
    /\ exitReason' = [exitReason EXCEPT ![t] = "interrupt"]
    /\ UNCHANGED <<atomic, interrupted, atomicFlips>>

-----------------------------------------------------------------------------

Next ==
    \/ \E t \in Threads : EnterWait(t)
    \/ FlipAtomic
    \/ \E t \in Threads : Interrupt(t)
    \/ \E t \in Threads : WakeOnAtomic(t)
    \/ \E t \in Threads : WakeOnInterrupt(t)

\* Per-thread strong fairness on the two wake actions. This pins down "if the wake source is
\* persistently enabled, the wake fires" -- exactly the property the bare-atomic wait violates.
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ \A t \in Threads :
         /\ SF_vars(WakeOnAtomic(t))
         /\ SF_vars(WakeOnInterrupt(t))
         /\ WF_vars(EnterWait(t))

-----------------------------------------------------------------------------
\* Safety
-----------------------------------------------------------------------------

TypeOK ==
    /\ state       \in [Threads -> States]
    /\ atomic      \in BOOLEAN
    /\ interrupted \in [Threads -> BOOLEAN]
    /\ atomicFlips \in 0..MaxAtomicFlips
    /\ exitReason  \in [Threads -> ExitReasons]

\* exitReason and state are kept consistent: a Done thread has a real exit reason.
DoneHasReason ==
    \A t \in Threads :
        (state[t] = "Done") => (exitReason[t] \in {"atomic", "interrupt"})

\* Non-Done threads have not yet recorded an exit reason.
NotDoneHasNoReason ==
    \A t \in Threads :
        (state[t] # "Done") => (exitReason[t] = "none")

\* The atomic flag is single-use per flip: only one waiter can consume each True->False edge.
\* (Captured by the fact that WakeOnAtomic resets atomic.)
AtomicConsumedExactlyOnce ==
    atomic \/ (Cardinality({t \in Threads : exitReason[t] = "atomic"}) <= atomicFlips)

-----------------------------------------------------------------------------
\* Liveness
-----------------------------------------------------------------------------

\* Headline liveness invariant: every waiter whose interrupt bit has been set
\* eventually leaves the wait. This is what `waitForAtomicOrInterrupt` must
\* guarantee for killOp / shutdown to bound the worst-case unblock latency.
InterruptIsResponsiveProp ==
    \A t \in Threads :
        (state[t] = "Waiting" /\ interrupted[t]) ~> (state[t] = "Done")

\* Companion: every waiter eventually exits the wait (either because a wake source fires
\* or because, in the no-interrupt configuration, the producer eventually wakes it).
\* In the bug configuration with no interrupt branch this becomes a stronger statement
\* than the system can deliver if the producer is starved.
WaitTerminates ==
    \A t \in Threads :
        (state[t] = "Waiting") ~> (state[t] = "Done")

=================================================================================================
