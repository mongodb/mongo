---------------------------- MODULE PriorityTicketHolder ----------------------------
\* This module contains the current implementation of the PriorityTicketHolder
\* Any modification performed there must be reflected here in order to prove its correctness.
\* The file can be found at src/mongo/util/concurrency/priority_ticketholder.cpp

EXTENDS Integers, Sequences, TLC, FiniteSets

CONSTANT InitialNumberOfTicketsAvailable
CONSTANT MaximumNumberOfWorkers

\* These are the possible futex values of TicketWaiter::State
None == -1
Waiting == 0
Acquired == 1
TimedOut == 2

(* --algorithm TicketHolder {

variables
    Proc = 1..MaximumNumberOfWorkers,
    ticketsAvailable = InitialNumberOfTicketsAvailable,
    mutexLocked = FALSE,
    \* Queue of just pids. Order is implementation-defined anyways so we can use a set.
    queuedProcs = {},
    \* This functions as a map of pids to waiter state. This must be manually initialized to have
    \* MaximumNumberOfWorkers elements.
    procState = <<None, None, None>>,
    \* This is the set of pids that are eligible for being signaled to timeout.
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
            Lock();
    innerMutexTry:
            if (ticketsAvailable > 0) {
                ticketsAvailable := ticketsAvailable - 1;
                Unlock(); 
                return;
            };
    registerAsWaiter:
            queuedProcs := queuedProcs \union {pid};
            procState[pid] := Waiting;
    doneRegistering:
            Unlock();
            \* We register the process as available for a random timeout. This set is modified
            \* by the Timeout process concurrently so a timeout can occur at any moment.
            procsForTimeout := procsForTimeout \union {pid};
    waitForTicketHandedOver:
            \* Wait until given a ticket (1st condition) or has timed out (2nd condition)
            await (procState[pid] = Acquired) \/ ~(pid \in procsForTimeout);
            hasTimedOut := ~(pid \in procsForTimeout);
            \* We've woken up, remove ourselves as available for timeout.
            procsForTimeout := procsForTimeout \ {pid};
    checkForTicketAfterTimeout:
            if (hasTimedOut) {
                hasTicket := (procState[pid] = Acquired);
                if (~hasTicket) {
                    \* We let the releasers know that we have timed out so that they do not give
                    \* us a ticket. They will remove us from the queue.
                    procState[pid] := TimedOut;
                };
            }
            else {
    ticketCheck:
                hasTicket := (procState[pid] = Acquired);
                if (hasTicket) {
                    queuedProcs := queuedProcs \ {pid};
                };
            };
        };
    done:
        return;
};

procedure Release()
variables chosenPid = -1;
{
    whileNonTimedOutWaiter:
        while (TRUE) {
            Lock();
    dequeueProc:
            if (queuedProcs # {}) {
                with (aPid \in queuedProcs) {
                    chosenPid := aPid;
                    queuedProcs := queuedProcs \ {chosenPid};
                };
    doneDequeueing:
                Unlock();
    notifyIfNotTimedOut:
                if (procState[chosenPid] /= TimedOut) {
                    procState[chosenPid] := Acquired;
                    return;
                };
            }
            else {
    releaseTicket:
                ticketsAvailable := ticketsAvailable + 1;
                Unlock();
                return;
            };
        };
    done:
        return;
};

fair process (Timeout = 0)
{
    timeoutLoop:
        while (TRUE) {
    acquirePid:
            if (queuedProcs # {}) {
                with (queuedPid \in queuedProcs) {
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
\* BEGIN TRANSLATION (chksum(pcal) = "3ca56f9e" /\ chksum(tla) = "ba80db81")
\* Label done of procedure Acquire at line 95 col 9 changed to done_
CONSTANT defaultInitValue
VARIABLES Proc, ticketsAvailable, mutexLocked, queuedProcs, procState, 
          procsForTimeout, pc, stack, pid, hasTicket, hasTimedOut, chosenPid, 
          shouldContinue

vars == << Proc, ticketsAvailable, mutexLocked, queuedProcs, procState, 
           procsForTimeout, pc, stack, pid, hasTicket, hasTimedOut, chosenPid, 
           shouldContinue >>

ProcSet == {0} \cup (Proc)

Init == (* Global variables *)
        /\ Proc = 1..MaximumNumberOfWorkers
        /\ ticketsAvailable = InitialNumberOfTicketsAvailable
        /\ mutexLocked = FALSE
        /\ queuedProcs = {}
        /\ procState = <<None, None, None>>
        /\ procsForTimeout = {}
        (* Procedure Acquire *)
        /\ pid = [ self \in ProcSet |-> defaultInitValue]
        /\ hasTicket = [ self \in ProcSet |-> FALSE]
        /\ hasTimedOut = [ self \in ProcSet |-> FALSE]
        (* Procedure Release *)
        /\ chosenPid = [ self \in ProcSet |-> -1]
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
                               /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
                               /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                               /\ UNCHANGED << ticketsAvailable, stack, pid, 
                                               hasTicket, hasTimedOut >>
                    /\ UNCHANGED << Proc, mutexLocked, queuedProcs, procState, 
                                    procsForTimeout, chosenPid, shouldContinue >>

enqueue(self) == /\ pc[self] = "enqueue"
                 /\ IF ~hasTicket[self]
                       THEN /\ mutexLocked = FALSE
                            /\ mutexLocked' = TRUE
                            /\ pc' = [pc EXCEPT ![self] = "innerMutexTry"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "done_"]
                            /\ UNCHANGED mutexLocked
                 /\ UNCHANGED << Proc, ticketsAvailable, queuedProcs, 
                                 procState, procsForTimeout, stack, pid, 
                                 hasTicket, hasTimedOut, chosenPid, 
                                 shouldContinue >>

innerMutexTry(self) == /\ pc[self] = "innerMutexTry"
                       /\ IF ticketsAvailable > 0
                             THEN /\ ticketsAvailable' = ticketsAvailable - 1
                                  /\ mutexLocked' = FALSE
                                  /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                  /\ hasTicket' = [hasTicket EXCEPT ![self] = Head(stack[self]).hasTicket]
                                  /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = Head(stack[self]).hasTimedOut]
                                  /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
                                  /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                             ELSE /\ pc' = [pc EXCEPT ![self] = "registerAsWaiter"]
                                  /\ UNCHANGED << ticketsAvailable, 
                                                  mutexLocked, stack, pid, 
                                                  hasTicket, hasTimedOut >>
                       /\ UNCHANGED << Proc, queuedProcs, procState, 
                                       procsForTimeout, chosenPid, 
                                       shouldContinue >>

registerAsWaiter(self) == /\ pc[self] = "registerAsWaiter"
                          /\ queuedProcs' = (queuedProcs \union {pid[self]})
                          /\ procState' = [procState EXCEPT ![pid[self]] = Waiting]
                          /\ pc' = [pc EXCEPT ![self] = "doneRegistering"]
                          /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, 
                                          procsForTimeout, stack, pid, 
                                          hasTicket, hasTimedOut, chosenPid, 
                                          shouldContinue >>

doneRegistering(self) == /\ pc[self] = "doneRegistering"
                         /\ mutexLocked' = FALSE
                         /\ procsForTimeout' = (procsForTimeout \union {pid[self]})
                         /\ pc' = [pc EXCEPT ![self] = "waitForTicketHandedOver"]
                         /\ UNCHANGED << Proc, ticketsAvailable, queuedProcs, 
                                         procState, stack, pid, hasTicket, 
                                         hasTimedOut, chosenPid, 
                                         shouldContinue >>

waitForTicketHandedOver(self) == /\ pc[self] = "waitForTicketHandedOver"
                                 /\ (procState[pid[self]] = Acquired) \/ ~(pid[self] \in procsForTimeout)
                                 /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = ~(pid[self] \in procsForTimeout)]
                                 /\ procsForTimeout' = procsForTimeout \ {pid[self]}
                                 /\ pc' = [pc EXCEPT ![self] = "checkForTicketAfterTimeout"]
                                 /\ UNCHANGED << Proc, ticketsAvailable, 
                                                 mutexLocked, queuedProcs, 
                                                 procState, stack, pid, 
                                                 hasTicket, chosenPid, 
                                                 shouldContinue >>

checkForTicketAfterTimeout(self) == /\ pc[self] = "checkForTicketAfterTimeout"
                                    /\ IF hasTimedOut[self]
                                          THEN /\ hasTicket' = [hasTicket EXCEPT ![self] = (procState[pid[self]] = Acquired)]
                                               /\ IF ~hasTicket'[self]
                                                     THEN /\ procState' = [procState EXCEPT ![pid[self]] = TimedOut]
                                                     ELSE /\ TRUE
                                                          /\ UNCHANGED procState
                                               /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                                          ELSE /\ pc' = [pc EXCEPT ![self] = "ticketCheck"]
                                               /\ UNCHANGED << procState, 
                                                               hasTicket >>
                                    /\ UNCHANGED << Proc, ticketsAvailable, 
                                                    mutexLocked, queuedProcs, 
                                                    procsForTimeout, stack, 
                                                    pid, hasTimedOut, 
                                                    chosenPid, shouldContinue >>

ticketCheck(self) == /\ pc[self] = "ticketCheck"
                     /\ hasTicket' = [hasTicket EXCEPT ![self] = (procState[pid[self]] = Acquired)]
                     /\ IF hasTicket'[self]
                           THEN /\ queuedProcs' = queuedProcs \ {pid[self]}
                           ELSE /\ TRUE
                                /\ UNCHANGED queuedProcs
                     /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                     /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, 
                                     procState, procsForTimeout, stack, pid, 
                                     hasTimedOut, chosenPid, shouldContinue >>

done_(self) == /\ pc[self] = "done_"
               /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
               /\ hasTicket' = [hasTicket EXCEPT ![self] = Head(stack[self]).hasTicket]
               /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = Head(stack[self]).hasTimedOut]
               /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
               /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
               /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, 
                               queuedProcs, procState, procsForTimeout, 
                               chosenPid, shouldContinue >>

Acquire(self) == tryAcquire(self) \/ enqueue(self) \/ innerMutexTry(self)
                    \/ registerAsWaiter(self) \/ doneRegistering(self)
                    \/ waitForTicketHandedOver(self)
                    \/ checkForTicketAfterTimeout(self)
                    \/ ticketCheck(self) \/ done_(self)

whileNonTimedOutWaiter(self) == /\ pc[self] = "whileNonTimedOutWaiter"
                                /\ mutexLocked = FALSE
                                /\ mutexLocked' = TRUE
                                /\ pc' = [pc EXCEPT ![self] = "dequeueProc"]
                                /\ UNCHANGED << Proc, ticketsAvailable, 
                                                queuedProcs, procState, 
                                                procsForTimeout, stack, pid, 
                                                hasTicket, hasTimedOut, 
                                                chosenPid, shouldContinue >>

dequeueProc(self) == /\ pc[self] = "dequeueProc"
                     /\ IF queuedProcs # {}
                           THEN /\ \E aPid \in queuedProcs:
                                     /\ chosenPid' = [chosenPid EXCEPT ![self] = aPid]
                                     /\ queuedProcs' = queuedProcs \ {chosenPid'[self]}
                                /\ pc' = [pc EXCEPT ![self] = "doneDequeueing"]
                           ELSE /\ pc' = [pc EXCEPT ![self] = "releaseTicket"]
                                /\ UNCHANGED << queuedProcs, chosenPid >>
                     /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, 
                                     procState, procsForTimeout, stack, pid, 
                                     hasTicket, hasTimedOut, shouldContinue >>

doneDequeueing(self) == /\ pc[self] = "doneDequeueing"
                        /\ mutexLocked' = FALSE
                        /\ pc' = [pc EXCEPT ![self] = "notifyIfNotTimedOut"]
                        /\ UNCHANGED << Proc, ticketsAvailable, queuedProcs, 
                                        procState, procsForTimeout, stack, pid, 
                                        hasTicket, hasTimedOut, chosenPid, 
                                        shouldContinue >>

notifyIfNotTimedOut(self) == /\ pc[self] = "notifyIfNotTimedOut"
                             /\ IF procState[chosenPid[self]] /= TimedOut
                                   THEN /\ procState' = [procState EXCEPT ![chosenPid[self]] = Acquired]
                                        /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                                        /\ chosenPid' = [chosenPid EXCEPT ![self] = Head(stack[self]).chosenPid]
                                        /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                                   ELSE /\ pc' = [pc EXCEPT ![self] = "whileNonTimedOutWaiter"]
                                        /\ UNCHANGED << procState, stack, 
                                                        chosenPid >>
                             /\ UNCHANGED << Proc, ticketsAvailable, 
                                             mutexLocked, queuedProcs, 
                                             procsForTimeout, pid, hasTicket, 
                                             hasTimedOut, shouldContinue >>

releaseTicket(self) == /\ pc[self] = "releaseTicket"
                       /\ ticketsAvailable' = ticketsAvailable + 1
                       /\ mutexLocked' = FALSE
                       /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                       /\ chosenPid' = [chosenPid EXCEPT ![self] = Head(stack[self]).chosenPid]
                       /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                       /\ UNCHANGED << Proc, queuedProcs, procState, 
                                       procsForTimeout, pid, hasTicket, 
                                       hasTimedOut, shouldContinue >>

done(self) == /\ pc[self] = "done"
              /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
              /\ chosenPid' = [chosenPid EXCEPT ![self] = Head(stack[self]).chosenPid]
              /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
              /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, queuedProcs, 
                              procState, procsForTimeout, pid, hasTicket, 
                              hasTimedOut, shouldContinue >>

Release(self) == whileNonTimedOutWaiter(self) \/ dequeueProc(self)
                    \/ doneDequeueing(self) \/ notifyIfNotTimedOut(self)
                    \/ releaseTicket(self) \/ done(self)

timeoutLoop == /\ pc[0] = "timeoutLoop"
               /\ pc' = [pc EXCEPT ![0] = "acquirePid"]
               /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, 
                               queuedProcs, procState, procsForTimeout, stack, 
                               pid, hasTicket, hasTimedOut, chosenPid, 
                               shouldContinue >>

acquirePid == /\ pc[0] = "acquirePid"
              /\ IF queuedProcs # {}
                    THEN /\ \E queuedPid \in queuedProcs:
                              procsForTimeout' = procsForTimeout \ {queuedPid}
                    ELSE /\ TRUE
                         /\ UNCHANGED procsForTimeout
              /\ pc' = [pc EXCEPT ![0] = "timeoutLoop"]
              /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, queuedProcs, 
                              procState, stack, pid, hasTicket, hasTimedOut, 
                              chosenPid, shouldContinue >>

Timeout == timeoutLoop \/ acquirePid

loop(self) == /\ pc[self] = "loop"
              /\ IF shouldContinue[self]
                    THEN /\ pc' = [pc EXCEPT ![self] = "acquire"]
                    ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
              /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, queuedProcs, 
                              procState, procsForTimeout, stack, pid, 
                              hasTicket, hasTimedOut, chosenPid, 
                              shouldContinue >>

acquire(self) == /\ pc[self] = "acquire"
                 /\ /\ pid' = [pid EXCEPT ![self] = self]
                    /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Acquire",
                                                             pc        |->  "release",
                                                             hasTicket |->  hasTicket[self],
                                                             hasTimedOut |->  hasTimedOut[self],
                                                             pid       |->  pid[self] ] >>
                                                         \o stack[self]]
                 /\ hasTicket' = [hasTicket EXCEPT ![self] = FALSE]
                 /\ hasTimedOut' = [hasTimedOut EXCEPT ![self] = FALSE]
                 /\ pc' = [pc EXCEPT ![self] = "tryAcquire"]
                 /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, 
                                 queuedProcs, procState, procsForTimeout, 
                                 chosenPid, shouldContinue >>

release(self) == /\ pc[self] = "release"
                 /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Release",
                                                          pc        |->  "maybeFinish",
                                                          chosenPid |->  chosenPid[self] ] >>
                                                      \o stack[self]]
                 /\ chosenPid' = [chosenPid EXCEPT ![self] = -1]
                 /\ pc' = [pc EXCEPT ![self] = "whileNonTimedOutWaiter"]
                 /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, 
                                 queuedProcs, procState, procsForTimeout, pid, 
                                 hasTicket, hasTimedOut, shouldContinue >>

maybeFinish(self) == /\ pc[self] = "maybeFinish"
                     /\ \/ /\ shouldContinue' = [shouldContinue EXCEPT ![self] = TRUE]
                        \/ /\ shouldContinue' = [shouldContinue EXCEPT ![self] = FALSE]
                     /\ pc' = [pc EXCEPT ![self] = "loop"]
                     /\ UNCHANGED << Proc, ticketsAvailable, mutexLocked, 
                                     queuedProcs, procState, procsForTimeout, 
                                     stack, pid, hasTicket, hasTimedOut, 
                                     chosenPid >>

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
