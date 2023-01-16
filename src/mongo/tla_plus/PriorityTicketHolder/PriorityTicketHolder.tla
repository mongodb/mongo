---------------------------- MODULE PriorityTicketHolder ----------------------------
\* This module contains the current implementation of the PriorityTicketHolder
\* Any modification performed there must be reflected here in order to prove its correctness.
\* The file can be found at src/mongo/util/concurrency/priority_ticketholder.cpp

EXTENDS Integers, Sequences, TLC, FiniteSets

CONSTANT InitialNumberOfTicketsAvailable
CONSTANT MaximumNumberOfWorkers

(* --algorithm TicketHolder {

variables
    Proc = 1..MaximumNumberOfWorkers,
    QueueIndexes = {1, 2},
    ticketsAvailable = InitialNumberOfTicketsAvailable,
    mutexLocked = FALSE,
    queuedProcs = <<{}, {}>>,
    numQueuedProc = <<0, 0>>,
    procsForTimeout = {};

macro Lock() {
    await mutexLocked = FALSE;
    mutexLocked := TRUE;
}

macro Unlock() {
    mutexLocked := FALSE;
}

procedure Acquire(pid)
variables 
    hasTicket = FALSE,
    hasTimedOut = FALSE,
    procQueue = -1 ;
{
    \* This is the tryAcquire() method inlined.
    tryAcquire:
        if (ticketsAvailable > 0) {
            ticketsAvailable := ticketsAvailable - 1;
            return;
        };

    \* The rest of the method is the enqueue and wait process.
    
    enqueue:
        while (~hasTicket) {
    acquireEnqueuerLock:
            Lock();
    innerMutexTry:
            if (ticketsAvailable > 0) {
                ticketsAvailable := ticketsAvailable - 1;
                Unlock(); 
                return;
            };
    registerAsWaiter:
            with (chosen \in QueueIndexes) {
                procQueue := chosen;
            };
            numQueuedProc[procQueue] := numQueuedProc[procQueue] + 1;
            queuedProcs[procQueue] := queuedProcs[procQueue] \union {pid};
    doneRegistering:
            Unlock();
            \* We register the process as available for a random timeout. This set is modified
            \* by the Timeout process concurrently so a timeout can occur at any moment.
            procsForTimeout := procsForTimeout \union {pid};
    waitForTicketHandedOver:
            \* Wait until given a ticket (1st condition) or has timed out (2nd condition)
            await ~(pid \in queuedProcs[procQueue]) \/ ~(pid \in procsForTimeout);
            hasTimedOut := ~(pid \in procsForTimeout);
            \* We've woken up, remove ourselves as available for timeout.
            procsForTimeout := procsForTimeout \ {pid};
    unregisterAsWaiter:
            if (hasTimedOut) {
    lockForUnregister:
                Lock();
    actualUnregister:
                hasTicket := ~(pid \in queuedProcs[procQueue]);
                if (~hasTicket) {
                    numQueuedProc[procQueue] := numQueuedProc[procQueue] - 1;
                };
                queuedProcs[procQueue] := queuedProcs[procQueue] \ {pid};
    unlockForUnregister:
                Unlock();
            } else {
    ticketCheck:
                hasTicket := ~(pid \in queuedProcs[procQueue]);
            };
        };
    done:
        return;
};

procedure Release()
variables triedQueues = {},
          chosenQueue = -1 ;
{
    disableEnqueueing:
        Lock();
    dequeueProc:
        while (triedQueues # QueueIndexes) {
            with (queueChosen \in (QueueIndexes \ triedQueues)) {
                chosenQueue := queueChosen;
            };
    checkWoken:
            if (numQueuedProc[chosenQueue] > 0) {
                numQueuedProc[chosenQueue] := numQueuedProc[chosenQueue] - 1;
                with (chosenPid \in queuedProcs[chosenQueue]) {
                    queuedProcs[chosenQueue] := queuedProcs[chosenQueue] \ {chosenPid};
                };
                goto done;
            };
    failedToDequeue:
            triedQueues := triedQueues \union {chosenQueue};           
        };
    releaseTicket:
        ticketsAvailable := ticketsAvailable + 1;
    done:
        Unlock();
        return;
};

fair process (Timeout = 0) {
    timeoutLoop:
        while (TRUE) {
    acquirePid:
            if (procsForTimeout # {}) {
                with (queuedPid \in procsForTimeout) {
                    procsForTimeout := procsForTimeout \ {queuedPid};
                };
            };
        };
};

fair process (P \in Proc)
variable shouldContinue = TRUE ;
{
    loop:
        while (shouldContinue) {
    acquire:
            call Acquire(self);
    release:
            call Release();
    maybeFinish:
            either {
                shouldContinue := TRUE;
            } or {
                shouldContinue := FALSE;
            };
        };
};
} *)
\* BEGIN TRANSLATION (chksum(pcal) = "8055c5bb" /\ chksum(tla) = "1c77803b")
\* Label done of procedure Acquire at line 91 col 9 changed to done_
CONSTANT defaultInitValue
VARIABLES Proc, QueueIndexes, ticketsAvailable, mutexLocked, queuedProcs, 
          numQueuedProc, procsForTimeout, pc, stack, pid, hasTicket, 
          hasTimedOut, procQueue, triedQueues, chosenQueue, shouldContinue

vars == << Proc, QueueIndexes, ticketsAvailable, mutexLocked, queuedProcs, 
           numQueuedProc, procsForTimeout, pc, stack, pid, hasTicket, 
           hasTimedOut, procQueue, triedQueues, chosenQueue, shouldContinue
        >>

ProcSet == {0} \cup (Proc)

Init == (* Global variables *)
        /\ Proc = 1..MaximumNumberOfWorkers
        /\ QueueIndexes = {1, 2}
        /\ ticketsAvailable = InitialNumberOfTicketsAvailable
        /\ mutexLocked = FALSE
        /\ queuedProcs = <<{}, {}>>
        /\ numQueuedProc = <<0, 0>>
        /\ procsForTimeout = {}
        (* Procedure Acquire *)
        /\ pid = [ self \in ProcSet |-> defaultInitValue]
        /\ hasTicket = [ self \in ProcSet |-> FALSE]
        /\ hasTimedOut = [ self \in ProcSet |-> FALSE]
        /\ procQueue = [ self \in ProcSet |-> -1]
        (* Procedure Release *)
        /\ triedQueues = [ self \in ProcSet |-> {}]
        /\ chosenQueue = [ self \in ProcSet |-> -1]
        (* Process P *)
        /\ shouldContinue = [self \in Proc |-> TRUE]
        /\ stack = [self \in ProcSet |-> << >>]
        /\ pc = [self \in ProcSet |-> CASE self = 0 -> "timeoutLoop"
                                        [] self \in Proc -> "loop"]

tryAcquire(self) == /\ pc[self] = "tryAcquire"
                    /\ IF ticketsAvailable > 0
                          THEN /\ ticketsAvailable' = ticketsAvailable - 1
                               /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                               /\ hasTicket' = [hasTicket EXCEPT ![self] = Head(stack[self]).hasTicket]
                               /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = Head(stack[self]).hasTimedOut]
                               /\ procQueue' = [procQueue EXCEPT ![self] = Head(stack[self]).procQueue]
                               /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
                               /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                               /\ UNCHANGED << ticketsAvailable, stack, pid, 
                                               hasTicket, hasTimedOut, 
                                               procQueue >>
                    /\ UNCHANGED << Proc, QueueIndexes, mutexLocked, 
                                    queuedProcs, numQueuedProc, 
                                    procsForTimeout, triedQueues, chosenQueue, 
                                    shouldContinue >>

enqueue(self) == /\ pc[self] = "enqueue"
                 /\ IF ~hasTicket[self]
                       THEN /\ pc' = [pc EXCEPT ![self] = "acquireEnqueuerLock"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "done_"]
                 /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                 mutexLocked, queuedProcs, numQueuedProc, 
                                 procsForTimeout, stack, pid, hasTicket, 
                                 hasTimedOut, procQueue, triedQueues, 
                                 chosenQueue, shouldContinue >>

acquireEnqueuerLock(self) == /\ pc[self] = "acquireEnqueuerLock"
                             /\ mutexLocked = FALSE
                             /\ mutexLocked' = TRUE
                             /\ pc' = [pc EXCEPT ![self] = "innerMutexTry"]
                             /\ UNCHANGED << Proc, QueueIndexes, 
                                             ticketsAvailable, queuedProcs, 
                                             numQueuedProc, procsForTimeout, 
                                             stack, pid, hasTicket, 
                                             hasTimedOut, procQueue, 
                                             triedQueues, chosenQueue, 
                                             shouldContinue >>

innerMutexTry(self) == /\ pc[self] = "innerMutexTry"
                       /\ IF ticketsAvailable > 0
                             THEN /\ ticketsAvailable' = ticketsAvailable - 1
                                  /\ mutexLocked' = FALSE
                                  /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                  /\ hasTicket' = [hasTicket EXCEPT ![self] = Head(stack[self]).hasTicket]
                                  /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = Head(stack[self]).hasTimedOut]
                                  /\ procQueue' = [procQueue EXCEPT ![self] = Head(stack[self]).procQueue]
                                  /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
                                  /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                             ELSE /\ pc' = [pc EXCEPT ![self] = "registerAsWaiter"]
                                  /\ UNCHANGED << ticketsAvailable, 
                                                  mutexLocked, stack, pid, 
                                                  hasTicket, hasTimedOut, 
                                                  procQueue >>
                       /\ UNCHANGED << Proc, QueueIndexes, queuedProcs, 
                                       numQueuedProc, procsForTimeout, 
                                       triedQueues, chosenQueue, 
                                       shouldContinue >>

registerAsWaiter(self) == /\ pc[self] = "registerAsWaiter"
                          /\ \E chosen \in QueueIndexes:
                               procQueue' = [procQueue EXCEPT ![self] = chosen]
                          /\ numQueuedProc' = [numQueuedProc EXCEPT ![procQueue'[self]] = numQueuedProc[procQueue'[self]] + 1]
                          /\ queuedProcs' = [queuedProcs EXCEPT ![procQueue'[self]] = queuedProcs[procQueue'[self]] \union {pid[self]}]
                          /\ pc' = [pc EXCEPT ![self] = "doneRegistering"]
                          /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                          mutexLocked, procsForTimeout, stack, 
                                          pid, hasTicket, hasTimedOut, 
                                          triedQueues, chosenQueue, 
                                          shouldContinue >>

doneRegistering(self) == /\ pc[self] = "doneRegistering"
                         /\ mutexLocked' = FALSE
                         /\ procsForTimeout' = (procsForTimeout \union {pid[self]})
                         /\ pc' = [pc EXCEPT ![self] = "waitForTicketHandedOver"]
                         /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                         queuedProcs, numQueuedProc, stack, 
                                         pid, hasTicket, hasTimedOut, 
                                         procQueue, triedQueues, chosenQueue, 
                                         shouldContinue >>

waitForTicketHandedOver(self) == /\ pc[self] = "waitForTicketHandedOver"
                                 /\ ~(pid[self] \in queuedProcs[procQueue[self]]) \/ ~(pid[self] \in procsForTimeout)
                                 /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = ~(pid[self] \in procsForTimeout)]
                                 /\ procsForTimeout' = procsForTimeout \ {pid[self]}
                                 /\ pc' = [pc EXCEPT ![self] = "unregisterAsWaiter"]
                                 /\ UNCHANGED << Proc, QueueIndexes, 
                                                 ticketsAvailable, mutexLocked, 
                                                 queuedProcs, numQueuedProc, 
                                                 stack, pid, hasTicket, 
                                                 procQueue, triedQueues, 
                                                 chosenQueue, shouldContinue >>

unregisterAsWaiter(self) == /\ pc[self] = "unregisterAsWaiter"
                            /\ IF hasTimedOut[self]
                                  THEN /\ pc' = [pc EXCEPT ![self] = "lockForUnregister"]
                                  ELSE /\ pc' = [pc EXCEPT ![self] = "ticketCheck"]
                            /\ UNCHANGED << Proc, QueueIndexes, 
                                            ticketsAvailable, mutexLocked, 
                                            queuedProcs, numQueuedProc, 
                                            procsForTimeout, stack, pid, 
                                            hasTicket, hasTimedOut, procQueue, 
                                            triedQueues, chosenQueue, 
                                            shouldContinue >>

lockForUnregister(self) == /\ pc[self] = "lockForUnregister"
                           /\ mutexLocked = FALSE
                           /\ mutexLocked' = TRUE
                           /\ pc' = [pc EXCEPT ![self] = "actualUnregister"]
                           /\ UNCHANGED << Proc, QueueIndexes, 
                                           ticketsAvailable, queuedProcs, 
                                           numQueuedProc, procsForTimeout, 
                                           stack, pid, hasTicket, hasTimedOut, 
                                           procQueue, triedQueues, chosenQueue, 
                                           shouldContinue >>

actualUnregister(self) == /\ pc[self] = "actualUnregister"
                          /\ hasTicket' = [hasTicket EXCEPT ![self] = ~(pid[self] \in queuedProcs[procQueue[self]])]
                          /\ IF ~hasTicket'[self]
                                THEN /\ numQueuedProc' = [numQueuedProc EXCEPT ![procQueue[self]] = numQueuedProc[procQueue[self]] - 1]
                                ELSE /\ TRUE
                                     /\ UNCHANGED numQueuedProc
                          /\ queuedProcs' = [queuedProcs EXCEPT ![procQueue[self]] = queuedProcs[procQueue[self]] \ {pid[self]}]
                          /\ pc' = [pc EXCEPT ![self] = "unlockForUnregister"]
                          /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                          mutexLocked, procsForTimeout, stack, 
                                          pid, hasTimedOut, procQueue, 
                                          triedQueues, chosenQueue, 
                                          shouldContinue >>

unlockForUnregister(self) == /\ pc[self] = "unlockForUnregister"
                             /\ mutexLocked' = FALSE
                             /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                             /\ UNCHANGED << Proc, QueueIndexes, 
                                             ticketsAvailable, queuedProcs, 
                                             numQueuedProc, procsForTimeout, 
                                             stack, pid, hasTicket, 
                                             hasTimedOut, procQueue, 
                                             triedQueues, chosenQueue, 
                                             shouldContinue >>

ticketCheck(self) == /\ pc[self] = "ticketCheck"
                     /\ hasTicket' = [hasTicket EXCEPT ![self] = ~(pid[self] \in queuedProcs[procQueue[self]])]
                     /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                     /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                     mutexLocked, queuedProcs, numQueuedProc, 
                                     procsForTimeout, stack, pid, hasTimedOut, 
                                     procQueue, triedQueues, chosenQueue, 
                                     shouldContinue >>

done_(self) == /\ pc[self] = "done_"
               /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
               /\ hasTicket' = [hasTicket EXCEPT ![self] = Head(stack[self]).hasTicket]
               /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = Head(stack[self]).hasTimedOut]
               /\ procQueue' = [procQueue EXCEPT ![self] = Head(stack[self]).procQueue]
               /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
               /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
               /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                               mutexLocked, queuedProcs, numQueuedProc, 
                               procsForTimeout, triedQueues, chosenQueue, 
                               shouldContinue >>

Acquire(self) == tryAcquire(self) \/ enqueue(self)
                    \/ acquireEnqueuerLock(self) \/ innerMutexTry(self)
                    \/ registerAsWaiter(self) \/ doneRegistering(self)
                    \/ waitForTicketHandedOver(self)
                    \/ unregisterAsWaiter(self) \/ lockForUnregister(self)
                    \/ actualUnregister(self) \/ unlockForUnregister(self)
                    \/ ticketCheck(self) \/ done_(self)

disableEnqueueing(self) == /\ pc[self] = "disableEnqueueing"
                           /\ mutexLocked = FALSE
                           /\ mutexLocked' = TRUE
                           /\ pc' = [pc EXCEPT ![self] = "dequeueProc"]
                           /\ UNCHANGED << Proc, QueueIndexes, 
                                           ticketsAvailable, queuedProcs, 
                                           numQueuedProc, procsForTimeout, 
                                           stack, pid, hasTicket, hasTimedOut, 
                                           procQueue, triedQueues, chosenQueue, 
                                           shouldContinue >>

dequeueProc(self) == /\ pc[self] = "dequeueProc"
                     /\ IF triedQueues[self] # QueueIndexes
                           THEN /\ \E queueChosen \in (QueueIndexes \ triedQueues[self]):
                                     chosenQueue' = [chosenQueue EXCEPT ![self] = queueChosen]
                                /\ pc' = [pc EXCEPT ![self] = "checkWoken"]
                           ELSE /\ pc' = [pc EXCEPT ![self] = "releaseTicket"]
                                /\ UNCHANGED chosenQueue
                     /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                     mutexLocked, queuedProcs, numQueuedProc, 
                                     procsForTimeout, stack, pid, hasTicket, 
                                     hasTimedOut, procQueue, triedQueues, 
                                     shouldContinue >>

checkWoken(self) == /\ pc[self] = "checkWoken"
                    /\ IF numQueuedProc[chosenQueue[self]] > 0
                          THEN /\ numQueuedProc' = [numQueuedProc EXCEPT ![chosenQueue[self]] = numQueuedProc[chosenQueue[self]] - 1]
                               /\ \E chosenPid \in queuedProcs[chosenQueue[self]]:
                                    queuedProcs' = [queuedProcs EXCEPT ![chosenQueue[self]] = queuedProcs[chosenQueue[self]] \ {chosenPid}]
                               /\ pc' = [pc EXCEPT ![self] = "done"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "failedToDequeue"]
                               /\ UNCHANGED << queuedProcs, numQueuedProc >>
                    /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                    mutexLocked, procsForTimeout, stack, pid, 
                                    hasTicket, hasTimedOut, procQueue, 
                                    triedQueues, chosenQueue, shouldContinue >>

failedToDequeue(self) == /\ pc[self] = "failedToDequeue"
                         /\ triedQueues' = [triedQueues EXCEPT ![self] = triedQueues[self] \union {chosenQueue[self]}]
                         /\ pc' = [pc EXCEPT ![self] = "dequeueProc"]
                         /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                         mutexLocked, queuedProcs, 
                                         numQueuedProc, procsForTimeout, stack, 
                                         pid, hasTicket, hasTimedOut, 
                                         procQueue, chosenQueue, 
                                         shouldContinue >>

releaseTicket(self) == /\ pc[self] = "releaseTicket"
                       /\ ticketsAvailable' = ticketsAvailable + 1
                       /\ pc' = [pc EXCEPT ![self] = "done"]
                       /\ UNCHANGED << Proc, QueueIndexes, mutexLocked, 
                                       queuedProcs, numQueuedProc, 
                                       procsForTimeout, stack, pid, hasTicket, 
                                       hasTimedOut, procQueue, triedQueues, 
                                       chosenQueue, shouldContinue >>

done(self) == /\ pc[self] = "done"
              /\ mutexLocked' = FALSE
              /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
              /\ triedQueues' = [triedQueues EXCEPT ![self] = Head(stack[self]).triedQueues]
              /\ chosenQueue' = [chosenQueue EXCEPT ![self] = Head(stack[self]).chosenQueue]
              /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
              /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                              queuedProcs, numQueuedProc, procsForTimeout, pid, 
                              hasTicket, hasTimedOut, procQueue, 
                              shouldContinue >>

Release(self) == disableEnqueueing(self) \/ dequeueProc(self)
                    \/ checkWoken(self) \/ failedToDequeue(self)
                    \/ releaseTicket(self) \/ done(self)

timeoutLoop == /\ pc[0] = "timeoutLoop"
               /\ pc' = [pc EXCEPT ![0] = "acquirePid"]
               /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                               mutexLocked, queuedProcs, numQueuedProc, 
                               procsForTimeout, stack, pid, hasTicket, 
                               hasTimedOut, procQueue, triedQueues, 
                               chosenQueue, shouldContinue >>

acquirePid == /\ pc[0] = "acquirePid"
              /\ IF procsForTimeout # {}
                    THEN /\ \E queuedPid \in procsForTimeout:
                              procsForTimeout' = procsForTimeout \ {queuedPid}
                    ELSE /\ TRUE
                         /\ UNCHANGED procsForTimeout
              /\ pc' = [pc EXCEPT ![0] = "timeoutLoop"]
              /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                              mutexLocked, queuedProcs, numQueuedProc, stack, 
                              pid, hasTicket, hasTimedOut, procQueue, 
                              triedQueues, chosenQueue, shouldContinue >>

Timeout == timeoutLoop \/ acquirePid

loop(self) == /\ pc[self] = "loop"
              /\ IF shouldContinue[self]
                    THEN /\ pc' = [pc EXCEPT ![self] = "acquire"]
                    ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
              /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                              mutexLocked, queuedProcs, numQueuedProc, 
                              procsForTimeout, stack, pid, hasTicket, 
                              hasTimedOut, procQueue, triedQueues, chosenQueue, 
                              shouldContinue >>

acquire(self) == /\ pc[self] = "acquire"
                 /\ /\ pid' = [pid EXCEPT ![self] = self]
                    /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Acquire",
                                                             pc        |->  "release",
                                                             hasTicket |->  hasTicket[self],
                                                             hasTimedOut |->  hasTimedOut[self],
                                                             procQueue |->  procQueue[self],
                                                             pid       |->  pid[self] ] >>
                                                         \o stack[self]]
                 /\ hasTicket' = [hasTicket EXCEPT ![self] = FALSE]
                 /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = FALSE]
                 /\ procQueue' = [procQueue EXCEPT ![self] = -1]
                 /\ pc' = [pc EXCEPT ![self] = "tryAcquire"]
                 /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                 mutexLocked, queuedProcs, numQueuedProc, 
                                 procsForTimeout, triedQueues, chosenQueue, 
                                 shouldContinue >>

release(self) == /\ pc[self] = "release"
                 /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Release",
                                                          pc        |->  "maybeFinish",
                                                          triedQueues |->  triedQueues[self],
                                                          chosenQueue |->  chosenQueue[self] ] >>
                                                      \o stack[self]]
                 /\ triedQueues' = [triedQueues EXCEPT ![self] = {}]
                 /\ chosenQueue' = [chosenQueue EXCEPT ![self] = -1]
                 /\ pc' = [pc EXCEPT ![self] = "disableEnqueueing"]
                 /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                 mutexLocked, queuedProcs, numQueuedProc, 
                                 procsForTimeout, pid, hasTicket, hasTimedOut, 
                                 procQueue, shouldContinue >>

maybeFinish(self) == /\ pc[self] = "maybeFinish"
                     /\ \/ /\ shouldContinue' = [shouldContinue EXCEPT ![self] = TRUE]
                        \/ /\ shouldContinue' = [shouldContinue EXCEPT ![self] = FALSE]
                     /\ pc' = [pc EXCEPT ![self] = "loop"]
                     /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                     mutexLocked, queuedProcs, numQueuedProc, 
                                     procsForTimeout, stack, pid, hasTicket, 
                                     hasTimedOut, procQueue, triedQueues, 
                                     chosenQueue >>

P(self) == loop(self) \/ acquire(self) \/ release(self)
              \/ maybeFinish(self)

Next == Timeout
           \/ (\E self \in ProcSet: Acquire(self) \/ Release(self))
           \/ (\E self \in Proc: P(self))

Spec == /\ Init /\ [][Next]_vars
        /\ WF_vars(Timeout)
        /\ \A self \in Proc : WF_vars(P(self)) /\ WF_vars(Acquire(self)) /\ WF_vars(Release(self))

\* END TRANSLATION 
============================
