\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------- MODULE OrderedTicketSemaphore ----------------------------------
\* Models the OrderedTicketSemaphore acquire/release protocol to demonstrate a deadlock if a 
\* ResizerClient takes two tickets consecutively.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Clients,                 \* Set of client threads.
          InitPermits,             \* Initial number of permits.
          ResizerClientMaxTickets, \* Max tickets the resizer client holds at once.
          ResizeDelta              \* Max tickets for an immediate resize operation.

ResizerClient == "ResizerClient"
AllClients == Clients \union {ResizerClient}

VARIABLES permits,      \* Available permits (integer).
          taken,        \* Taken permits (integer).
          waitQueue,    \* Sequence of [thread |-> t, awake |-> BOOLEAN].
          heldTickets   \* Tickets held per client.

vars == <<permits, taken, waitQueue, heldTickets>>

ASSUME InitPermits >= ResizerClientMaxTickets

-----------------------------------------------------------------------------

\* Index of the first non-awake waiter, or 0 if all are awake.
FirstNonAwake(q) ==
    LET indices == {i \in 1..Len(q) : ~q[i].awake}
    IN IF indices = {} THEN 0
       ELSE CHOOSE i \in indices : \A j \in indices : i <= j

\* Remove element at position idx from sequence s.
RemoveAt(s, idx) ==
    SubSeq(s, 1, idx - 1) \o SubSeq(s, idx + 1, Len(s))

\* Index of the waiter record for thread t, or 0 if not found.
WaiterIndex(q, t) ==
    LET indices == {i \in 1..Len(q) : q[i].thread = t}
    IN IF indices = {} THEN 0
       ELSE CHOOSE i \in indices : TRUE

Min(a, b) == IF a < b THEN a ELSE b

\* Return q after awakening the first N non-awake clients.
WakeFirstN(q, n) ==
    LET first == FirstNonAwake(q)
        toWake == Min(n, Len(q) - first + 1)
    IN IF first = 0 \/ n <= 0 THEN q
       ELSE [i \in 1..Len(q) |->
               IF i >= first /\ i < first + toWake
               THEN [q[i] EXCEPT !.awake = TRUE]
               ELSE q[i]]

IsWaiting(t) ==
    WaiterIndex(waitQueue, t) > 0

IsWaitingUninterruptibly(t) ==
    /\ IsWaiting(t)
    /\ LET idx == WaiterIndex(waitQueue, t) 
       IN ~waitQueue[idx].interruptible 

CanAcquire(t) ==
    /\ ~IsWaiting(t)
    /\ IF t = ResizerClient
       THEN heldTickets[t] < ResizerClientMaxTickets \* Limit for ResizerClient.
       ELSE heldTickets[t] = 0                       \* Normal clients can only acquire if not holding.

IsHolding(t) ==
    heldTickets[t] > 0

\* Fast path, take ticket right away without queueing.
CanSkipQueue ==
    permits > Len(waitQueue)
    \* permits > 0 /\ waitQueue = <<>> \* SERVER-122680, causes deadlock/liveness failures.

-----------------------------------------------------------------------------

Init ==
    /\ permits = InitPermits
    /\ taken = 0
    /\ waitQueue = <<>>
    /\ heldTickets = [t \in AllClients |-> 0]

\* Client t attempts to acquire a permit via the fast path.
TryAcquire(t) ==
    /\ CanAcquire(t)
    /\ CanSkipQueue
    /\ permits' = permits - 1
    /\ taken' = taken + 1
    /\ heldTickets' = [heldTickets EXCEPT ![t] = heldTickets[t] + 1]
    /\ UNCHANGED waitQueue

\* Client t queues to acquire a permit via the slow path.
Acquire(t, interruptible) ==
    /\ CanAcquire(t)
    /\ ~CanSkipQueue
    /\ waitQueue' = Append(waitQueue, [thread |-> t, awake |-> FALSE, interruptible |-> interruptible])
    /\ UNCHANGED <<permits, heldTickets, taken>>

\* Woken waiter t consumes a permit and exits the queue.
WakeAndConsume(t) ==
    /\ IsWaiting(t)
    /\ LET idx == WaiterIndex(waitQueue, t)
       IN /\ idx > 0
          /\ waitQueue[idx].awake = TRUE
          /\ waitQueue' = RemoveAt(waitQueue, idx)
    /\ permits' = permits - 1
    /\ taken' = taken + 1
    /\ heldTickets' = [heldTickets EXCEPT ![t] = heldTickets[t] + 1]

\* Client t releases its permit and wakes the first sleeping waiter if one exists.
DoRelease(t) ==
    /\ IsHolding(t)
    /\ ~IsWaiting(t)
    /\ permits' = permits + 1
    /\ taken' = taken - 1
    /\ heldTickets' = [heldTickets EXCEPT ![t] = heldTickets[t] - 1]
    /\ IF permits' > 0 
       THEN waitQueue' = WakeFirstN(waitQueue, 1)
       ELSE UNCHANGED waitQueue

\* We change the number of permits immediately, instead of acquiring or releasing tickets
\* one by one.
ImmediateResize(n) == 
    /\ ~IsHolding(ResizerClient)
    /\ ~IsWaiting(ResizerClient)
    /\ permits + taken + n >= ResizerClientMaxTickets \* At least enough tickets for the ResizerClient.
    /\ permits' = permits + n
    /\ waitQueue' = WakeFirstN(waitQueue, permits')
    /\ UNCHANGED <<heldTickets, taken>>

\* A waiting thread is interrupted. If it had been woken already, it must cascade the 
\* wake up to avoid a lost signal.
Interrupt(t) ==
    /\ IsWaiting(t)
    /\ LET idx == WaiterIndex(waitQueue, t)
       IN /\ idx > 0
          /\ waitQueue[idx].interruptible
          /\ IF waitQueue[idx].awake
             THEN waitQueue' = WakeFirstN(RemoveAt(waitQueue, idx), 1)
             ELSE waitQueue' = RemoveAt(waitQueue, idx)
    /\ UNCHANGED <<permits, taken, heldTickets>>

-----------------------------------------------------------------------------

Next == \/ \E t \in AllClients : TryAcquire(t)  
        \/ \E t \in AllClients : \E interruptible \in BOOLEAN: Acquire(t, interruptible)  
        \/ \E t \in AllClients : WakeAndConsume(t)  
        \/ \E t \in AllClients : DoRelease(t)  
        \/ \E t \in AllClients : Interrupt(t)  
        \* ResizerClient can also immediately resize.  
        \/ \E n \in -ResizeDelta..ResizeDelta: ImmediateResize(n)

Spec == /\ Init 
        /\ [][Next]_vars 
        /\ WF_vars(Next)
        /\ \A t \in AllClients : /\ WF_vars(WakeAndConsume(t))
                                 /\ WF_vars(DoRelease(t))

-----------------------------------------------------------------------------
\* Invariants
-----------------------------------------------------------------------------

TypeOK ==
    /\ permits \in Int
    /\ taken \in Int
    /\ \A i \in 1..Len(waitQueue) :
        /\ waitQueue[i].thread \in AllClients
        /\ waitQueue[i].awake \in BOOLEAN
        /\ waitQueue[i].interruptible \in BOOLEAN
    /\ heldTickets \in [AllClients -> Int]

\* Taken is always non-negative.
TakenNonNegative == taken >= 0

\* Taken equals the number of permits actually held across all threads.
TakenMatchesHolding ==
    LET holdingCount[S \in SUBSET AllClients] ==
            IF S = {} THEN 0
            ELSE LET t == CHOOSE x \in S : TRUE
                 IN heldTickets[t] + holdingCount[S \ {t}]
    IN taken = holdingCount[AllClients]

\* Each thread appears at most once in the queue.
UniqueWaiters ==
    \A i, j \in 1..Len(waitQueue) : i # j => waitQueue[i].thread # waitQueue[j].thread

\* Awake entries precede sleeping entries in the queue.
AwakeBeforeNonAwake ==
    \A i, j \in 1..Len(waitQueue) :
        (i < j /\ ~waitQueue[i].awake) => ~waitQueue[j].awake

-----------------------------------------------------------------------------
\* Liveness
-----------------------------------------------------------------------------

\* A client that is waiting uninterruptibly will eventually hold a permit.
WaitingLeadsToHolding == \A t \in AllClients :
    IsWaitingUninterruptibly(t) ~> IsHolding(t)

\* A client never waits indefinitely, it eventually acquires a ticket or is interrupted.
NeverWaitsIndefinitely == \A t \in AllClients :
    IsWaiting(t) ~> (IsHolding(t) \/ ~IsWaiting(t))
=================================================================================================
