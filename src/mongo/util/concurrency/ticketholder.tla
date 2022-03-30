---------------------------- MODULE ticketholder ----------------------------
EXTENDS Integers, Sequences, TLC, FiniteSets

\* This is a PlusCal program, to execute it load it up into TLA+ Toolbox and modify it there.

(****************

--algorithm TicketHolder {

variables
          Proc = {1, 2, 3},
          \* The waiting queue is abstracted as a set of pids waiting to be dequeued.
          \* This way we are independent of any queueing implementation and policy.
          Waiters = {},
          ticketsAvailable = 2,
          ticketsEnqueued = 0,
          ActiveProc = Proc,
          queueBeingModified = FALSE,
          ModifyingState = <<FALSE, FALSE, FALSE>>,
          IsWaiting = <<FALSE, FALSE, FALSE>>;

\* You can pack functionality into macros and it will substitute invocations
\* with the steps inside wherever they are invoked
macro dequeueIntoVariable(queue, var) {
    \* This selects one element of the set non-deterministically
    with (elem \in queue) {
        queue := queue \ {elem} ;
        var := elem;
    }
};

\* The current implementation relies on having a mutex surrounding a queue so that
\* only one process can modify the queue. This is a verifiably correct way of not having deadlocks.

procedure Acquire(pid)
variables localTicketsAvailableAcquire = -1,
          localTicketsEnqueuedAcquire = -1 ;
{
    \* This is the tryAcquire() method inlined.
    lockAttemptCopy:
        localTicketsEnqueuedAcquire := ticketsEnqueued ;
    attempt:
        if (localTicketsEnqueuedAcquire > 0) {
            goto enqueue;
        };
    copyTickets:
        ticketsAvailable := ticketsAvailable - 1;
        localTicketsAvailableAcquire := ticketsAvailable ;
    attemptOptimistic:
        if (localTicketsAvailableAcquire < 0) {
    failedOptimistic:
            ticketsAvailable := ticketsAvailable + 1;
            goto enqueue;
        } else {
    successOptimistic:
            return;
        };

    \* The rest of the method is the enqueue and wait process.
    enqueue:
        await queueBeingModified = FALSE;
        queueBeingModified := TRUE;
    modifyEnqueued:
        ticketsEnqueued := ticketsEnqueued + 1 ;
    modifyAvailableOptimistically:
        ticketsAvailable := ticketsAvailable - 1;
        localTicketsAvailableAcquire := ticketsAvailable;
    optimisticCheckQueue:
        if (localTicketsAvailableAcquire >= 0) {
    checkSucceeded:
            ticketsEnqueued := ticketsEnqueued - 1;
            queueBeingModified := FALSE;
            return;
        };
    pessimisticRelease:
        ticketsAvailable := ticketsAvailable + 1;
    actualEnqueue:
        Waiters := Waiters \union {pid};
        IsWaiting[pid] := TRUE;
        queueBeingModified := FALSE;
    resolve:
        await IsWaiting[pid] = FALSE /\ ModifyingState[pid] = FALSE;
        return;
};

procedure Release()
variables localTicketsAvailable = -1,
          localTicketsEnqueued = -1,
          dequeuedElem = -1 ;
{
    disableEnqueueing:
        await queueBeingModified = FALSE;
        queueBeingModified := TRUE;
    release:
        while (TRUE) {
    dequeue:
            if (Waiters # {}) {
                dequeueIntoVariable(Waiters, dequeuedElem) ;
    dequeued:
                ticketsEnqueued := ticketsEnqueued - 1;
    modifyStateLock:
                await ModifyingState[dequeuedElem] = FALSE;
                ModifyingState[dequeuedElem] := TRUE;
    modifyState:
                if (~ IsWaiting[dequeuedElem]) {
                    ModifyingState[dequeuedElem] := FALSE;
                    goto dequeue;
                };
    wakeWaiter:
                IsWaiting[dequeuedElem] := FALSE;
                ModifyingState[dequeuedElem] := FALSE;
            } else {
    releaseTicket:
                ticketsAvailable := ticketsAvailable + 1;
            };
    finished:
            queueBeingModified := FALSE;
            return;
        };
};

process (P \in Proc)
variable localPid = -1 ;
{
    loop:
        while (TRUE) {
            dequeueIntoVariable(ActiveProc, localPid);
    acquire:
            call Acquire(localPid);
    release:
            call Release();
    releasePid:
            ActiveProc := ActiveProc \union {localPid};
        };
};
}

***************)
\* BEGIN TRANSLATION (chksum(pcal) = "2bdace43" /\ chksum(tla) = "c60322e0")
\* Label release of procedure Release at line 95 col 9 changed to release_
CONSTANT defaultInitValue
VARIABLES Proc, Waiters, ticketsAvailable, ticketsEnqueued, ActiveProc,
          queueBeingModified, ModifyingState, IsWaiting, pc, stack, pid,
          localTicketsAvailableAcquire, localTicketsEnqueuedAcquire,
          localTicketsAvailable, localTicketsEnqueued, dequeuedElem, localPid

vars == << Proc, Waiters, ticketsAvailable, ticketsEnqueued, ActiveProc,
           queueBeingModified, ModifyingState, IsWaiting, pc, stack, pid,
           localTicketsAvailableAcquire, localTicketsEnqueuedAcquire,
           localTicketsAvailable, localTicketsEnqueued, dequeuedElem,
           localPid >>

ProcSet == (Proc)

Init == (* Global variables *)
        /\ Proc = {1, 2, 3}
        /\ Waiters = {}
        /\ ticketsAvailable = 2
        /\ ticketsEnqueued = 0
        /\ ActiveProc = Proc
        /\ queueBeingModified = FALSE
        /\ ModifyingState = <<FALSE, FALSE, FALSE>>
        /\ IsWaiting = <<FALSE, FALSE, FALSE>>
        (* Procedure Acquire *)
        /\ pid = [ self \in ProcSet |-> defaultInitValue]
        /\ localTicketsAvailableAcquire = [ self \in ProcSet |-> -1]
        /\ localTicketsEnqueuedAcquire = [ self \in ProcSet |-> -1]
        (* Procedure Release *)
        /\ localTicketsAvailable = [ self \in ProcSet |-> -1]
        /\ localTicketsEnqueued = [ self \in ProcSet |-> -1]
        /\ dequeuedElem = [ self \in ProcSet |-> -1]
        (* Process P *)
        /\ localPid = [self \in Proc |-> -1]
        /\ stack = [self \in ProcSet |-> << >>]
        /\ pc = [self \in ProcSet |-> "loop"]

lockAttemptCopy(self) == /\ pc[self] = "lockAttemptCopy"
                         /\ localTicketsEnqueuedAcquire' = [localTicketsEnqueuedAcquire EXCEPT ![self] = ticketsEnqueued]
                         /\ pc' = [pc EXCEPT ![self] = "attempt"]
                         /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                         ticketsEnqueued, ActiveProc,
                                         queueBeingModified, ModifyingState,
                                         IsWaiting, stack, pid,
                                         localTicketsAvailableAcquire,
                                         localTicketsAvailable,
                                         localTicketsEnqueued, dequeuedElem,
                                         localPid >>

attempt(self) == /\ pc[self] = "attempt"
                 /\ IF localTicketsEnqueuedAcquire[self] > 0
                       THEN /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "copyTickets"]
                 /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                 ticketsEnqueued, ActiveProc,
                                 queueBeingModified, ModifyingState, IsWaiting,
                                 stack, pid, localTicketsAvailableAcquire,
                                 localTicketsEnqueuedAcquire,
                                 localTicketsAvailable, localTicketsEnqueued,
                                 dequeuedElem, localPid >>

copyTickets(self) == /\ pc[self] = "copyTickets"
                     /\ ticketsAvailable' = ticketsAvailable - 1
                     /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = ticketsAvailable']
                     /\ pc' = [pc EXCEPT ![self] = "attemptOptimistic"]
                     /\ UNCHANGED << Proc, Waiters, ticketsEnqueued,
                                     ActiveProc, queueBeingModified,
                                     ModifyingState, IsWaiting, stack, pid,
                                     localTicketsEnqueuedAcquire,
                                     localTicketsAvailable,
                                     localTicketsEnqueued, dequeuedElem,
                                     localPid >>

attemptOptimistic(self) == /\ pc[self] = "attemptOptimistic"
                           /\ IF localTicketsAvailableAcquire[self] < 0
                                 THEN /\ pc' = [pc EXCEPT ![self] = "failedOptimistic"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "successOptimistic"]
                           /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                           ticketsEnqueued, ActiveProc,
                                           queueBeingModified, ModifyingState,
                                           IsWaiting, stack, pid,
                                           localTicketsAvailableAcquire,
                                           localTicketsEnqueuedAcquire,
                                           localTicketsAvailable,
                                           localTicketsEnqueued, dequeuedElem,
                                           localPid >>

failedOptimistic(self) == /\ pc[self] = "failedOptimistic"
                          /\ ticketsAvailable' = ticketsAvailable + 1
                          /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                          /\ UNCHANGED << Proc, Waiters, ticketsEnqueued,
                                          ActiveProc, queueBeingModified,
                                          ModifyingState, IsWaiting, stack,
                                          pid, localTicketsAvailableAcquire,
                                          localTicketsEnqueuedAcquire,
                                          localTicketsAvailable,
                                          localTicketsEnqueued, dequeuedElem,
                                          localPid >>

successOptimistic(self) == /\ pc[self] = "successOptimistic"
                           /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                           /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = Head(stack[self]).localTicketsAvailableAcquire]
                           /\ localTicketsEnqueuedAcquire' = [localTicketsEnqueuedAcquire EXCEPT ![self] = Head(stack[self]).localTicketsEnqueuedAcquire]
                           /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
                           /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                           /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                           ticketsEnqueued, ActiveProc,
                                           queueBeingModified, ModifyingState,
                                           IsWaiting, localTicketsAvailable,
                                           localTicketsEnqueued, dequeuedElem,
                                           localPid >>

enqueue(self) == /\ pc[self] = "enqueue"
                 /\ queueBeingModified = FALSE
                 /\ queueBeingModified' = TRUE
                 /\ pc' = [pc EXCEPT ![self] = "modifyEnqueued"]
                 /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                 ticketsEnqueued, ActiveProc, ModifyingState,
                                 IsWaiting, stack, pid,
                                 localTicketsAvailableAcquire,
                                 localTicketsEnqueuedAcquire,
                                 localTicketsAvailable, localTicketsEnqueued,
                                 dequeuedElem, localPid >>

modifyEnqueued(self) == /\ pc[self] = "modifyEnqueued"
                        /\ ticketsEnqueued' = ticketsEnqueued + 1
                        /\ pc' = [pc EXCEPT ![self] = "modifyAvailableOptimistically"]
                        /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                        ActiveProc, queueBeingModified,
                                        ModifyingState, IsWaiting, stack, pid,
                                        localTicketsAvailableAcquire,
                                        localTicketsEnqueuedAcquire,
                                        localTicketsAvailable,
                                        localTicketsEnqueued, dequeuedElem,
                                        localPid >>

modifyAvailableOptimistically(self) == /\ pc[self] = "modifyAvailableOptimistically"
                                       /\ ticketsAvailable' = ticketsAvailable - 1
                                       /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = ticketsAvailable']
                                       /\ pc' = [pc EXCEPT ![self] = "optimisticCheckQueue"]
                                       /\ UNCHANGED << Proc, Waiters,
                                                       ticketsEnqueued,
                                                       ActiveProc,
                                                       queueBeingModified,
                                                       ModifyingState,
                                                       IsWaiting, stack, pid,
                                                       localTicketsEnqueuedAcquire,
                                                       localTicketsAvailable,
                                                       localTicketsEnqueued,
                                                       dequeuedElem, localPid >>

optimisticCheckQueue(self) == /\ pc[self] = "optimisticCheckQueue"
                              /\ IF localTicketsAvailableAcquire[self] >= 0
                                    THEN /\ pc' = [pc EXCEPT ![self] = "checkSucceeded"]
                                    ELSE /\ pc' = [pc EXCEPT ![self] = "pessimisticRelease"]
                              /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                              ticketsEnqueued, ActiveProc,
                                              queueBeingModified,
                                              ModifyingState, IsWaiting, stack,
                                              pid,
                                              localTicketsAvailableAcquire,
                                              localTicketsEnqueuedAcquire,
                                              localTicketsAvailable,
                                              localTicketsEnqueued,
                                              dequeuedElem, localPid >>

checkSucceeded(self) == /\ pc[self] = "checkSucceeded"
                        /\ ticketsEnqueued' = ticketsEnqueued - 1
                        /\ queueBeingModified' = FALSE
                        /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                        /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = Head(stack[self]).localTicketsAvailableAcquire]
                        /\ localTicketsEnqueuedAcquire' = [localTicketsEnqueuedAcquire EXCEPT ![self] = Head(stack[self]).localTicketsEnqueuedAcquire]
                        /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
                        /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                        /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                        ActiveProc, ModifyingState, IsWaiting,
                                        localTicketsAvailable,
                                        localTicketsEnqueued, dequeuedElem,
                                        localPid >>

pessimisticRelease(self) == /\ pc[self] = "pessimisticRelease"
                            /\ ticketsAvailable' = ticketsAvailable + 1
                            /\ pc' = [pc EXCEPT ![self] = "actualEnqueue"]
                            /\ UNCHANGED << Proc, Waiters, ticketsEnqueued,
                                            ActiveProc, queueBeingModified,
                                            ModifyingState, IsWaiting, stack,
                                            pid, localTicketsAvailableAcquire,
                                            localTicketsEnqueuedAcquire,
                                            localTicketsAvailable,
                                            localTicketsEnqueued, dequeuedElem,
                                            localPid >>

actualEnqueue(self) == /\ pc[self] = "actualEnqueue"
                       /\ Waiters' = (Waiters \union {pid[self]})
                       /\ IsWaiting' = [IsWaiting EXCEPT ![pid[self]] = TRUE]
                       /\ queueBeingModified' = FALSE
                       /\ pc' = [pc EXCEPT ![self] = "resolve"]
                       /\ UNCHANGED << Proc, ticketsAvailable, ticketsEnqueued,
                                       ActiveProc, ModifyingState, stack, pid,
                                       localTicketsAvailableAcquire,
                                       localTicketsEnqueuedAcquire,
                                       localTicketsAvailable,
                                       localTicketsEnqueued, dequeuedElem,
                                       localPid >>

resolve(self) == /\ pc[self] = "resolve"
                 /\ IsWaiting[pid[self]] = FALSE /\ ModifyingState[pid[self]] = FALSE
                 /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                 /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = Head(stack[self]).localTicketsAvailableAcquire]
                 /\ localTicketsEnqueuedAcquire' = [localTicketsEnqueuedAcquire EXCEPT ![self] = Head(stack[self]).localTicketsEnqueuedAcquire]
                 /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
                 /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                 /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                 ticketsEnqueued, ActiveProc,
                                 queueBeingModified, ModifyingState, IsWaiting,
                                 localTicketsAvailable, localTicketsEnqueued,
                                 dequeuedElem, localPid >>

Acquire(self) == lockAttemptCopy(self) \/ attempt(self)
                    \/ copyTickets(self) \/ attemptOptimistic(self)
                    \/ failedOptimistic(self) \/ successOptimistic(self)
                    \/ enqueue(self) \/ modifyEnqueued(self)
                    \/ modifyAvailableOptimistically(self)
                    \/ optimisticCheckQueue(self) \/ checkSucceeded(self)
                    \/ pessimisticRelease(self) \/ actualEnqueue(self)
                    \/ resolve(self)

disableEnqueueing(self) == /\ pc[self] = "disableEnqueueing"
                           /\ queueBeingModified = FALSE
                           /\ queueBeingModified' = TRUE
                           /\ pc' = [pc EXCEPT ![self] = "release_"]
                           /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                           ticketsEnqueued, ActiveProc,
                                           ModifyingState, IsWaiting, stack,
                                           pid, localTicketsAvailableAcquire,
                                           localTicketsEnqueuedAcquire,
                                           localTicketsAvailable,
                                           localTicketsEnqueued, dequeuedElem,
                                           localPid >>

release_(self) == /\ pc[self] = "release_"
                  /\ pc' = [pc EXCEPT ![self] = "dequeue"]
                  /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                  ticketsEnqueued, ActiveProc,
                                  queueBeingModified, ModifyingState,
                                  IsWaiting, stack, pid,
                                  localTicketsAvailableAcquire,
                                  localTicketsEnqueuedAcquire,
                                  localTicketsAvailable, localTicketsEnqueued,
                                  dequeuedElem, localPid >>

dequeue(self) == /\ pc[self] = "dequeue"
                 /\ IF Waiters # {}
                       THEN /\ \E elem \in Waiters:
                                 /\ Waiters' = Waiters \ {elem}
                                 /\ dequeuedElem' = [dequeuedElem EXCEPT ![self] = elem]
                            /\ pc' = [pc EXCEPT ![self] = "dequeued"]
                       ELSE /\ pc' = [pc EXCEPT ![self] = "releaseTicket"]
                            /\ UNCHANGED << Waiters, dequeuedElem >>
                 /\ UNCHANGED << Proc, ticketsAvailable, ticketsEnqueued,
                                 ActiveProc, queueBeingModified,
                                 ModifyingState, IsWaiting, stack, pid,
                                 localTicketsAvailableAcquire,
                                 localTicketsEnqueuedAcquire,
                                 localTicketsAvailable, localTicketsEnqueued,
                                 localPid >>

dequeued(self) == /\ pc[self] = "dequeued"
                  /\ ticketsEnqueued' = ticketsEnqueued - 1
                  /\ pc' = [pc EXCEPT ![self] = "modifyStateLock"]
                  /\ UNCHANGED << Proc, Waiters, ticketsAvailable, ActiveProc,
                                  queueBeingModified, ModifyingState,
                                  IsWaiting, stack, pid,
                                  localTicketsAvailableAcquire,
                                  localTicketsEnqueuedAcquire,
                                  localTicketsAvailable, localTicketsEnqueued,
                                  dequeuedElem, localPid >>

modifyStateLock(self) == /\ pc[self] = "modifyStateLock"
                         /\ ModifyingState[dequeuedElem[self]] = FALSE
                         /\ ModifyingState' = [ModifyingState EXCEPT ![dequeuedElem[self]] = TRUE]
                         /\ pc' = [pc EXCEPT ![self] = "modifyState"]
                         /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                         ticketsEnqueued, ActiveProc,
                                         queueBeingModified, IsWaiting, stack,
                                         pid, localTicketsAvailableAcquire,
                                         localTicketsEnqueuedAcquire,
                                         localTicketsAvailable,
                                         localTicketsEnqueued, dequeuedElem,
                                         localPid >>

modifyState(self) == /\ pc[self] = "modifyState"
                     /\ IF ~ IsWaiting[dequeuedElem[self]]
                           THEN /\ ModifyingState' = [ModifyingState EXCEPT ![dequeuedElem[self]] = FALSE]
                                /\ pc' = [pc EXCEPT ![self] = "dequeue"]
                           ELSE /\ pc' = [pc EXCEPT ![self] = "wakeWaiter"]
                                /\ UNCHANGED ModifyingState
                     /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                     ticketsEnqueued, ActiveProc,
                                     queueBeingModified, IsWaiting, stack, pid,
                                     localTicketsAvailableAcquire,
                                     localTicketsEnqueuedAcquire,
                                     localTicketsAvailable,
                                     localTicketsEnqueued, dequeuedElem,
                                     localPid >>

wakeWaiter(self) == /\ pc[self] = "wakeWaiter"
                    /\ IsWaiting' = [IsWaiting EXCEPT ![dequeuedElem[self]] = FALSE]
                    /\ ModifyingState' = [ModifyingState EXCEPT ![dequeuedElem[self]] = FALSE]
                    /\ pc' = [pc EXCEPT ![self] = "finished"]
                    /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                    ticketsEnqueued, ActiveProc,
                                    queueBeingModified, stack, pid,
                                    localTicketsAvailableAcquire,
                                    localTicketsEnqueuedAcquire,
                                    localTicketsAvailable,
                                    localTicketsEnqueued, dequeuedElem,
                                    localPid >>

releaseTicket(self) == /\ pc[self] = "releaseTicket"
                       /\ ticketsAvailable' = ticketsAvailable + 1
                       /\ pc' = [pc EXCEPT ![self] = "finished"]
                       /\ UNCHANGED << Proc, Waiters, ticketsEnqueued,
                                       ActiveProc, queueBeingModified,
                                       ModifyingState, IsWaiting, stack, pid,
                                       localTicketsAvailableAcquire,
                                       localTicketsEnqueuedAcquire,
                                       localTicketsAvailable,
                                       localTicketsEnqueued, dequeuedElem,
                                       localPid >>

finished(self) == /\ pc[self] = "finished"
                  /\ queueBeingModified' = FALSE
                  /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                  /\ localTicketsAvailable' = [localTicketsAvailable EXCEPT ![self] = Head(stack[self]).localTicketsAvailable]
                  /\ localTicketsEnqueued' = [localTicketsEnqueued EXCEPT ![self] = Head(stack[self]).localTicketsEnqueued]
                  /\ dequeuedElem' = [dequeuedElem EXCEPT ![self] = Head(stack[self]).dequeuedElem]
                  /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                  /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                  ticketsEnqueued, ActiveProc, ModifyingState,
                                  IsWaiting, pid, localTicketsAvailableAcquire,
                                  localTicketsEnqueuedAcquire, localPid >>

Release(self) == disableEnqueueing(self) \/ release_(self) \/ dequeue(self)
                    \/ dequeued(self) \/ modifyStateLock(self)
                    \/ modifyState(self) \/ wakeWaiter(self)
                    \/ releaseTicket(self) \/ finished(self)

loop(self) == /\ pc[self] = "loop"
              /\ \E elem \in ActiveProc:
                   /\ ActiveProc' = ActiveProc \ {elem}
                   /\ localPid' = [localPid EXCEPT ![self] = elem]
              /\ pc' = [pc EXCEPT ![self] = "acquire"]
              /\ UNCHANGED << Proc, Waiters, ticketsAvailable, ticketsEnqueued,
                              queueBeingModified, ModifyingState, IsWaiting,
                              stack, pid, localTicketsAvailableAcquire,
                              localTicketsEnqueuedAcquire,
                              localTicketsAvailable, localTicketsEnqueued,
                              dequeuedElem >>

acquire(self) == /\ pc[self] = "acquire"
                 /\ /\ pid' = [pid EXCEPT ![self] = localPid[self]]
                    /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Acquire",
                                                             pc        |->  "release",
                                                             localTicketsAvailableAcquire |->  localTicketsAvailableAcquire[self],
                                                             localTicketsEnqueuedAcquire |->  localTicketsEnqueuedAcquire[self],
                                                             pid       |->  pid[self] ] >>
                                                         \o stack[self]]
                 /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = -1]
                 /\ localTicketsEnqueuedAcquire' = [localTicketsEnqueuedAcquire EXCEPT ![self] = -1]
                 /\ pc' = [pc EXCEPT ![self] = "lockAttemptCopy"]
                 /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                 ticketsEnqueued, ActiveProc,
                                 queueBeingModified, ModifyingState, IsWaiting,
                                 localTicketsAvailable, localTicketsEnqueued,
                                 dequeuedElem, localPid >>

release(self) == /\ pc[self] = "release"
                 /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Release",
                                                          pc        |->  "releasePid",
                                                          localTicketsAvailable |->  localTicketsAvailable[self],
                                                          localTicketsEnqueued |->  localTicketsEnqueued[self],
                                                          dequeuedElem |->  dequeuedElem[self] ] >>
                                                      \o stack[self]]
                 /\ localTicketsAvailable' = [localTicketsAvailable EXCEPT ![self] = -1]
                 /\ localTicketsEnqueued' = [localTicketsEnqueued EXCEPT ![self] = -1]
                 /\ dequeuedElem' = [dequeuedElem EXCEPT ![self] = -1]
                 /\ pc' = [pc EXCEPT ![self] = "disableEnqueueing"]
                 /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                 ticketsEnqueued, ActiveProc,
                                 queueBeingModified, ModifyingState, IsWaiting,
                                 pid, localTicketsAvailableAcquire,
                                 localTicketsEnqueuedAcquire, localPid >>

releasePid(self) == /\ pc[self] = "releasePid"
                    /\ ActiveProc' = (ActiveProc \union {localPid[self]})
                    /\ pc' = [pc EXCEPT ![self] = "loop"]
                    /\ UNCHANGED << Proc, Waiters, ticketsAvailable,
                                    ticketsEnqueued, queueBeingModified,
                                    ModifyingState, IsWaiting, stack, pid,
                                    localTicketsAvailableAcquire,
                                    localTicketsEnqueuedAcquire,
                                    localTicketsAvailable,
                                    localTicketsEnqueued, dequeuedElem,
                                    localPid >>

P(self) == loop(self) \/ acquire(self) \/ release(self) \/ releasePid(self)

Next == (\E self \in ProcSet: Acquire(self) \/ Release(self))
           \/ (\E self \in Proc: P(self))

Spec == Init /\ [][Next]_vars

\* END TRANSLATION

=============================================================================
\* Modification History
\* Last modified Tue Mar 29 12:58:58 CEST 2022 by jordi.olivares-provencio
\* Created Tue Mar 08 16:36:45 CET 2022 by jordi.olivares-provencio
