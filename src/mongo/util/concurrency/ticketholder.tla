---------------------------- MODULE ticketholder ----------------------------
EXTENDS Integers, Sequences, TLC, FiniteSets

\* This is a PlusCal program, to execute it load it up into TLA+ Toolbox and modify it there.
\* It contains a PlusCal model of the SchedulingTicketHolder.

(****************

--algorithm TicketHolder {

variables
          Proc = {1, 2, 3},
          QueueIndexes = {1, 2},
          ticketsAvailable = 2,
          ActiveProc = Proc,
          exclusiveLockQueue = FALSE,
          sharedLockQueue = 0,
          queuedWoken = <<0, 0>>,
          queuedProc = <<{}, {}>>,
          numQueuedProc = <<0, 0>>;

procedure Acquire(pid)
variables localTicketsAvailableAcquire = -1,
          procQueue = -1 ;
{
    \* This is the tryAcquire() method inlined.
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
        await exclusiveLockQueue = FALSE /\ sharedLockQueue = 0;
        exclusiveLockQueue := TRUE;
        with (chosen \in QueueIndexes) {
            procQueue := chosen;
        };
        numQueuedProc[procQueue] := numQueuedProc[procQueue] + 1;
    enqueueLoop:
        while (TRUE) {
    awaitTicketsLoop:
            while (ticketsAvailable <= 0) {
                exclusiveLockQueue := FALSE;
                queuedProc[procQueue] := queuedProc[procQueue] \union {pid};
    awaitWoken:
                await ~(pid \in queuedProc[procQueue]) /\ exclusiveLockQueue = FALSE /\ sharedLockQueue = 0;
                exclusiveLockQueue := TRUE;
    signalWokenThread:
                if (queuedWoken[procQueue] > 0) {
                    queuedWoken[procQueue] := queuedWoken[procQueue] - 1;
                };
            };
    attemptInnerTicketAcq:
            ticketsAvailable := ticketsAvailable - 1;
            localTicketsAvailableAcquire := ticketsAvailable ;
    innerCheck:
            if (localTicketsAvailableAcquire < 0) {
    failedCheck:
                ticketsAvailable := ticketsAvailable + 1;                
            } else {
                goto done;
            };
        };
    done:
        exclusiveLockQueue := FALSE;
        numQueuedProc[procQueue] := numQueuedProc[procQueue] - 1;
        return;
};

procedure Release()
variables localTicketsAvailable = -1,
          triedQueues = {},
          chosenQueue = -1 ;
{
    disableEnqueueing:
        await exclusiveLockQueue = FALSE;
        sharedLockQueue := sharedLockQueue + 1;
    releaseTicket:
        ticketsAvailable := ticketsAvailable + 1;
    earlyExit:
        if (queuedProc[1] = {} /\ queuedProc[2] = {}) {
            goto done;
        };
    dequeueProc:
        while (triedQueues # QueueIndexes) {
            with (queueChosen \in (QueueIndexes \ triedQueues)) {
                chosenQueue := queueChosen;
            };
    checkWoken:
            if (queuedWoken[chosenQueue] < numQueuedProc[chosenQueue]) {
                queuedWoken[chosenQueue] := queuedWoken[chosenQueue] + 1;
                with (queuedPid \in queuedProc[chosenQueue]) {
                    queuedProc[chosenQueue] := queuedProc[chosenQueue] \ {queuedPid};
                };
                goto done;
            };
    failedToDequeue:
            triedQueues := triedQueues \union {chosenQueue};           
        };
    done:
        sharedLockQueue := sharedLockQueue - 1;
        return;
};

fair process (P \in Proc)
variable iterationsLeft = 5 ;
{
    loop:
        while (iterationsLeft > 0) {
    acquire:
            call Acquire(self);
    release:
            call Release();
    decrementIterations:
            iterationsLeft := iterationsLeft - 1;
        };
};
}

***************)
\* BEGIN TRANSLATION (chksum(pcal) = "74f1edb6" /\ chksum(tla) = "8ad2fac8")
\* Label done of procedure Acquire at line 75 col 9 changed to done_
CONSTANT defaultInitValue
VARIABLES Proc, QueueIndexes, ticketsAvailable, ActiveProc, 
          exclusiveLockQueue, sharedLockQueue, queuedWoken, queuedProc, 
          numQueuedProc, pc, stack, pid, localTicketsAvailableAcquire, 
          procQueue, localTicketsAvailable, triedQueues, chosenQueue, 
          iterationsLeft

vars == << Proc, QueueIndexes, ticketsAvailable, ActiveProc, 
           exclusiveLockQueue, sharedLockQueue, queuedWoken, queuedProc, 
           numQueuedProc, pc, stack, pid, localTicketsAvailableAcquire, 
           procQueue, localTicketsAvailable, triedQueues, chosenQueue, 
           iterationsLeft >>

ProcSet == (Proc)

Init == (* Global variables *)
        /\ Proc = {1, 2, 3}
        /\ QueueIndexes = {1, 2}
        /\ ticketsAvailable = 2
        /\ ActiveProc = Proc
        /\ exclusiveLockQueue = FALSE
        /\ sharedLockQueue = 0
        /\ queuedWoken = <<0, 0>>
        /\ queuedProc = <<{}, {}>>
        /\ numQueuedProc = <<0, 0>>
        (* Procedure Acquire *)
        /\ pid = [ self \in ProcSet |-> defaultInitValue]
        /\ localTicketsAvailableAcquire = [ self \in ProcSet |-> -1]
        /\ procQueue = [ self \in ProcSet |-> -1]
        (* Procedure Release *)
        /\ localTicketsAvailable = [ self \in ProcSet |-> -1]
        /\ triedQueues = [ self \in ProcSet |-> {}]
        /\ chosenQueue = [ self \in ProcSet |-> -1]
        (* Process P *)
        /\ iterationsLeft = [self \in Proc |-> 5]
        /\ stack = [self \in ProcSet |-> << >>]
        /\ pc = [self \in ProcSet |-> "loop"]

copyTickets(self) == /\ pc[self] = "copyTickets"
                     /\ ticketsAvailable' = ticketsAvailable - 1
                     /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = ticketsAvailable']
                     /\ pc' = [pc EXCEPT ![self] = "attemptOptimistic"]
                     /\ UNCHANGED << Proc, QueueIndexes, ActiveProc, 
                                     exclusiveLockQueue, sharedLockQueue, 
                                     queuedWoken, queuedProc, numQueuedProc, 
                                     stack, pid, procQueue, 
                                     localTicketsAvailable, triedQueues, 
                                     chosenQueue, iterationsLeft >>

attemptOptimistic(self) == /\ pc[self] = "attemptOptimistic"
                           /\ IF localTicketsAvailableAcquire[self] < 0
                                 THEN /\ pc' = [pc EXCEPT ![self] = "failedOptimistic"]
                                 ELSE /\ pc' = [pc EXCEPT ![self] = "successOptimistic"]
                           /\ UNCHANGED << Proc, QueueIndexes, 
                                           ticketsAvailable, ActiveProc, 
                                           exclusiveLockQueue, sharedLockQueue, 
                                           queuedWoken, queuedProc, 
                                           numQueuedProc, stack, pid, 
                                           localTicketsAvailableAcquire, 
                                           procQueue, localTicketsAvailable, 
                                           triedQueues, chosenQueue, 
                                           iterationsLeft >>

failedOptimistic(self) == /\ pc[self] = "failedOptimistic"
                          /\ ticketsAvailable' = ticketsAvailable + 1
                          /\ pc' = [pc EXCEPT ![self] = "enqueue"]
                          /\ UNCHANGED << Proc, QueueIndexes, ActiveProc, 
                                          exclusiveLockQueue, sharedLockQueue, 
                                          queuedWoken, queuedProc, 
                                          numQueuedProc, stack, pid, 
                                          localTicketsAvailableAcquire, 
                                          procQueue, localTicketsAvailable, 
                                          triedQueues, chosenQueue, 
                                          iterationsLeft >>

successOptimistic(self) == /\ pc[self] = "successOptimistic"
                           /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
                           /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = Head(stack[self]).localTicketsAvailableAcquire]
                           /\ procQueue' = [procQueue EXCEPT ![self] = Head(stack[self]).procQueue]
                           /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
                           /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
                           /\ UNCHANGED << Proc, QueueIndexes, 
                                           ticketsAvailable, ActiveProc, 
                                           exclusiveLockQueue, sharedLockQueue, 
                                           queuedWoken, queuedProc, 
                                           numQueuedProc, 
                                           localTicketsAvailable, triedQueues, 
                                           chosenQueue, iterationsLeft >>

enqueue(self) == /\ pc[self] = "enqueue"
                 /\ exclusiveLockQueue = FALSE /\ sharedLockQueue = 0
                 /\ exclusiveLockQueue' = TRUE
                 /\ \E chosen \in QueueIndexes:
                      procQueue' = [procQueue EXCEPT ![self] = chosen]
                 /\ numQueuedProc' = [numQueuedProc EXCEPT ![procQueue'[self]] = numQueuedProc[procQueue'[self]] + 1]
                 /\ pc' = [pc EXCEPT ![self] = "enqueueLoop"]
                 /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                 ActiveProc, sharedLockQueue, queuedWoken, 
                                 queuedProc, stack, pid, 
                                 localTicketsAvailableAcquire, 
                                 localTicketsAvailable, triedQueues, 
                                 chosenQueue, iterationsLeft >>

enqueueLoop(self) == /\ pc[self] = "enqueueLoop"
                     /\ pc' = [pc EXCEPT ![self] = "awaitTicketsLoop"]
                     /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                     ActiveProc, exclusiveLockQueue, 
                                     sharedLockQueue, queuedWoken, queuedProc, 
                                     numQueuedProc, stack, pid, 
                                     localTicketsAvailableAcquire, procQueue, 
                                     localTicketsAvailable, triedQueues, 
                                     chosenQueue, iterationsLeft >>

awaitTicketsLoop(self) == /\ pc[self] = "awaitTicketsLoop"
                          /\ IF ticketsAvailable <= 0
                                THEN /\ exclusiveLockQueue' = FALSE
                                     /\ queuedProc' = [queuedProc EXCEPT ![procQueue[self]] = queuedProc[procQueue[self]] \union {pid[self]}]
                                     /\ pc' = [pc EXCEPT ![self] = "awaitWoken"]
                                ELSE /\ pc' = [pc EXCEPT ![self] = "attemptInnerTicketAcq"]
                                     /\ UNCHANGED << exclusiveLockQueue, 
                                                     queuedProc >>
                          /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                          ActiveProc, sharedLockQueue, 
                                          queuedWoken, numQueuedProc, stack, 
                                          pid, localTicketsAvailableAcquire, 
                                          procQueue, localTicketsAvailable, 
                                          triedQueues, chosenQueue, 
                                          iterationsLeft >>

awaitWoken(self) == /\ pc[self] = "awaitWoken"
                    /\ ~(pid[self] \in queuedProc[procQueue[self]]) /\ exclusiveLockQueue = FALSE /\ sharedLockQueue = 0
                    /\ exclusiveLockQueue' = TRUE
                    /\ pc' = [pc EXCEPT ![self] = "signalWokenThread"]
                    /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                    ActiveProc, sharedLockQueue, queuedWoken, 
                                    queuedProc, numQueuedProc, stack, pid, 
                                    localTicketsAvailableAcquire, procQueue, 
                                    localTicketsAvailable, triedQueues, 
                                    chosenQueue, iterationsLeft >>

signalWokenThread(self) == /\ pc[self] = "signalWokenThread"
                           /\ IF queuedWoken[procQueue[self]] > 0
                                 THEN /\ queuedWoken' = [queuedWoken EXCEPT ![procQueue[self]] = queuedWoken[procQueue[self]] - 1]
                                 ELSE /\ TRUE
                                      /\ UNCHANGED queuedWoken
                           /\ pc' = [pc EXCEPT ![self] = "awaitTicketsLoop"]
                           /\ UNCHANGED << Proc, QueueIndexes, 
                                           ticketsAvailable, ActiveProc, 
                                           exclusiveLockQueue, sharedLockQueue, 
                                           queuedProc, numQueuedProc, stack, 
                                           pid, localTicketsAvailableAcquire, 
                                           procQueue, localTicketsAvailable, 
                                           triedQueues, chosenQueue, 
                                           iterationsLeft >>

attemptInnerTicketAcq(self) == /\ pc[self] = "attemptInnerTicketAcq"
                               /\ ticketsAvailable' = ticketsAvailable - 1
                               /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = ticketsAvailable']
                               /\ pc' = [pc EXCEPT ![self] = "innerCheck"]
                               /\ UNCHANGED << Proc, QueueIndexes, ActiveProc, 
                                               exclusiveLockQueue, 
                                               sharedLockQueue, queuedWoken, 
                                               queuedProc, numQueuedProc, 
                                               stack, pid, procQueue, 
                                               localTicketsAvailable, 
                                               triedQueues, chosenQueue, 
                                               iterationsLeft >>

innerCheck(self) == /\ pc[self] = "innerCheck"
                    /\ IF localTicketsAvailableAcquire[self] < 0
                          THEN /\ pc' = [pc EXCEPT ![self] = "failedCheck"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "done_"]
                    /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                    ActiveProc, exclusiveLockQueue, 
                                    sharedLockQueue, queuedWoken, queuedProc, 
                                    numQueuedProc, stack, pid, 
                                    localTicketsAvailableAcquire, procQueue, 
                                    localTicketsAvailable, triedQueues, 
                                    chosenQueue, iterationsLeft >>

failedCheck(self) == /\ pc[self] = "failedCheck"
                     /\ ticketsAvailable' = ticketsAvailable + 1
                     /\ pc' = [pc EXCEPT ![self] = "enqueueLoop"]
                     /\ UNCHANGED << Proc, QueueIndexes, ActiveProc, 
                                     exclusiveLockQueue, sharedLockQueue, 
                                     queuedWoken, queuedProc, numQueuedProc, 
                                     stack, pid, localTicketsAvailableAcquire, 
                                     procQueue, localTicketsAvailable, 
                                     triedQueues, chosenQueue, iterationsLeft >>

done_(self) == /\ pc[self] = "done_"
               /\ exclusiveLockQueue' = FALSE
               /\ numQueuedProc' = [numQueuedProc EXCEPT ![procQueue[self]] = numQueuedProc[procQueue[self]] - 1]
               /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
               /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = Head(stack[self]).localTicketsAvailableAcquire]
               /\ procQueue' = [procQueue EXCEPT ![self] = Head(stack[self]).procQueue]
               /\ pid' = [pid EXCEPT ![self] = Head(stack[self]).pid]
               /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
               /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                               ActiveProc, sharedLockQueue, queuedWoken, 
                               queuedProc, localTicketsAvailable, triedQueues, 
                               chosenQueue, iterationsLeft >>

Acquire(self) == copyTickets(self) \/ attemptOptimistic(self)
                    \/ failedOptimistic(self) \/ successOptimistic(self)
                    \/ enqueue(self) \/ enqueueLoop(self)
                    \/ awaitTicketsLoop(self) \/ awaitWoken(self)
                    \/ signalWokenThread(self)
                    \/ attemptInnerTicketAcq(self) \/ innerCheck(self)
                    \/ failedCheck(self) \/ done_(self)

disableEnqueueing(self) == /\ pc[self] = "disableEnqueueing"
                           /\ exclusiveLockQueue = FALSE
                           /\ sharedLockQueue' = sharedLockQueue + 1
                           /\ pc' = [pc EXCEPT ![self] = "releaseTicket"]
                           /\ UNCHANGED << Proc, QueueIndexes, 
                                           ticketsAvailable, ActiveProc, 
                                           exclusiveLockQueue, queuedWoken, 
                                           queuedProc, numQueuedProc, stack, 
                                           pid, localTicketsAvailableAcquire, 
                                           procQueue, localTicketsAvailable, 
                                           triedQueues, chosenQueue, 
                                           iterationsLeft >>

releaseTicket(self) == /\ pc[self] = "releaseTicket"
                       /\ ticketsAvailable' = ticketsAvailable + 1
                       /\ pc' = [pc EXCEPT ![self] = "earlyExit"]
                       /\ UNCHANGED << Proc, QueueIndexes, ActiveProc, 
                                       exclusiveLockQueue, sharedLockQueue, 
                                       queuedWoken, queuedProc, numQueuedProc, 
                                       stack, pid, 
                                       localTicketsAvailableAcquire, procQueue, 
                                       localTicketsAvailable, triedQueues, 
                                       chosenQueue, iterationsLeft >>

earlyExit(self) == /\ pc[self] = "earlyExit"
                   /\ IF queuedProc[1] = {} /\ queuedProc[2] = {}
                         THEN /\ pc' = [pc EXCEPT ![self] = "done"]
                         ELSE /\ pc' = [pc EXCEPT ![self] = "dequeueProc"]
                   /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                   ActiveProc, exclusiveLockQueue, 
                                   sharedLockQueue, queuedWoken, queuedProc, 
                                   numQueuedProc, stack, pid, 
                                   localTicketsAvailableAcquire, procQueue, 
                                   localTicketsAvailable, triedQueues, 
                                   chosenQueue, iterationsLeft >>

dequeueProc(self) == /\ pc[self] = "dequeueProc"
                     /\ IF triedQueues[self] # QueueIndexes
                           THEN /\ \E queueChosen \in (QueueIndexes \ triedQueues[self]):
                                     chosenQueue' = [chosenQueue EXCEPT ![self] = queueChosen]
                                /\ pc' = [pc EXCEPT ![self] = "checkWoken"]
                           ELSE /\ pc' = [pc EXCEPT ![self] = "done"]
                                /\ UNCHANGED chosenQueue
                     /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                     ActiveProc, exclusiveLockQueue, 
                                     sharedLockQueue, queuedWoken, queuedProc, 
                                     numQueuedProc, stack, pid, 
                                     localTicketsAvailableAcquire, procQueue, 
                                     localTicketsAvailable, triedQueues, 
                                     iterationsLeft >>

checkWoken(self) == /\ pc[self] = "checkWoken"
                    /\ IF queuedWoken[chosenQueue[self]] < numQueuedProc[chosenQueue[self]]
                          THEN /\ queuedWoken' = [queuedWoken EXCEPT ![chosenQueue[self]] = queuedWoken[chosenQueue[self]] + 1]
                               /\ \E queuedPid \in queuedProc[chosenQueue[self]]:
                                    queuedProc' = [queuedProc EXCEPT ![chosenQueue[self]] = queuedProc[chosenQueue[self]] \ {queuedPid}]
                               /\ pc' = [pc EXCEPT ![self] = "done"]
                          ELSE /\ pc' = [pc EXCEPT ![self] = "failedToDequeue"]
                               /\ UNCHANGED << queuedWoken, queuedProc >>
                    /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                    ActiveProc, exclusiveLockQueue, 
                                    sharedLockQueue, numQueuedProc, stack, pid, 
                                    localTicketsAvailableAcquire, procQueue, 
                                    localTicketsAvailable, triedQueues, 
                                    chosenQueue, iterationsLeft >>

failedToDequeue(self) == /\ pc[self] = "failedToDequeue"
                         /\ triedQueues' = [triedQueues EXCEPT ![self] = triedQueues[self] \union {chosenQueue[self]}]
                         /\ pc' = [pc EXCEPT ![self] = "dequeueProc"]
                         /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                         ActiveProc, exclusiveLockQueue, 
                                         sharedLockQueue, queuedWoken, 
                                         queuedProc, numQueuedProc, stack, pid, 
                                         localTicketsAvailableAcquire, 
                                         procQueue, localTicketsAvailable, 
                                         chosenQueue, iterationsLeft >>

done(self) == /\ pc[self] = "done"
              /\ sharedLockQueue' = sharedLockQueue - 1
              /\ pc' = [pc EXCEPT ![self] = Head(stack[self]).pc]
              /\ localTicketsAvailable' = [localTicketsAvailable EXCEPT ![self] = Head(stack[self]).localTicketsAvailable]
              /\ triedQueues' = [triedQueues EXCEPT ![self] = Head(stack[self]).triedQueues]
              /\ chosenQueue' = [chosenQueue EXCEPT ![self] = Head(stack[self]).chosenQueue]
              /\ stack' = [stack EXCEPT ![self] = Tail(stack[self])]
              /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, ActiveProc, 
                              exclusiveLockQueue, queuedWoken, queuedProc, 
                              numQueuedProc, pid, localTicketsAvailableAcquire, 
                              procQueue, iterationsLeft >>

Release(self) == disableEnqueueing(self) \/ releaseTicket(self)
                    \/ earlyExit(self) \/ dequeueProc(self)
                    \/ checkWoken(self) \/ failedToDequeue(self)
                    \/ done(self)

loop(self) == /\ pc[self] = "loop"
              /\ IF iterationsLeft[self] > 0
                    THEN /\ pc' = [pc EXCEPT ![self] = "acquire"]
                    ELSE /\ pc' = [pc EXCEPT ![self] = "Done"]
              /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, ActiveProc, 
                              exclusiveLockQueue, sharedLockQueue, queuedWoken, 
                              queuedProc, numQueuedProc, stack, pid, 
                              localTicketsAvailableAcquire, procQueue, 
                              localTicketsAvailable, triedQueues, chosenQueue, 
                              iterationsLeft >>

acquire(self) == /\ pc[self] = "acquire"
                 /\ /\ pid' = [pid EXCEPT ![self] = self]
                    /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Acquire",
                                                             pc        |->  "release",
                                                             localTicketsAvailableAcquire |->  localTicketsAvailableAcquire[self],
                                                             procQueue |->  procQueue[self],
                                                             pid       |->  pid[self] ] >>
                                                         \o stack[self]]
                 /\ localTicketsAvailableAcquire' = [localTicketsAvailableAcquire EXCEPT ![self] = -1]
                 /\ procQueue' = [procQueue EXCEPT ![self] = -1]
                 /\ pc' = [pc EXCEPT ![self] = "copyTickets"]
                 /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                 ActiveProc, exclusiveLockQueue, 
                                 sharedLockQueue, queuedWoken, queuedProc, 
                                 numQueuedProc, localTicketsAvailable, 
                                 triedQueues, chosenQueue, iterationsLeft >>

release(self) == /\ pc[self] = "release"
                 /\ stack' = [stack EXCEPT ![self] = << [ procedure |->  "Release",
                                                          pc        |->  "decrementIterations",
                                                          localTicketsAvailable |->  localTicketsAvailable[self],
                                                          triedQueues |->  triedQueues[self],
                                                          chosenQueue |->  chosenQueue[self] ] >>
                                                      \o stack[self]]
                 /\ localTicketsAvailable' = [localTicketsAvailable EXCEPT ![self] = -1]
                 /\ triedQueues' = [triedQueues EXCEPT ![self] = {}]
                 /\ chosenQueue' = [chosenQueue EXCEPT ![self] = -1]
                 /\ pc' = [pc EXCEPT ![self] = "disableEnqueueing"]
                 /\ UNCHANGED << Proc, QueueIndexes, ticketsAvailable, 
                                 ActiveProc, exclusiveLockQueue, 
                                 sharedLockQueue, queuedWoken, queuedProc, 
                                 numQueuedProc, pid, 
                                 localTicketsAvailableAcquire, procQueue, 
                                 iterationsLeft >>

decrementIterations(self) == /\ pc[self] = "decrementIterations"
                             /\ iterationsLeft' = [iterationsLeft EXCEPT ![self] = iterationsLeft[self] - 1]
                             /\ pc' = [pc EXCEPT ![self] = "loop"]
                             /\ UNCHANGED << Proc, QueueIndexes, 
                                             ticketsAvailable, ActiveProc, 
                                             exclusiveLockQueue, 
                                             sharedLockQueue, queuedWoken, 
                                             queuedProc, numQueuedProc, stack, 
                                             pid, localTicketsAvailableAcquire, 
                                             procQueue, localTicketsAvailable, 
                                             triedQueues, chosenQueue >>

P(self) == loop(self) \/ acquire(self) \/ release(self)
              \/ decrementIterations(self)

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == /\ \A self \in ProcSet: pc[self] = "Done"
               /\ UNCHANGED vars

Next == (\E self \in ProcSet: Acquire(self) \/ Release(self))
           \/ (\E self \in Proc: P(self))
           \/ Terminating

Spec == /\ Init /\ [][Next]_vars
        /\ \A self \in Proc : WF_vars(P(self)) /\ WF_vars(Acquire(self)) /\ WF_vars(Release(self))

Termination == <>(\A self \in ProcSet: pc[self] = "Done")

\* END TRANSLATION

=============================================================================
\* Modification History
\* Last modified Wed Aug 17 17:50:49 CEST 2022 by jordi.olivares-provencio
\* Created Tue Mar 08 16:36:45 CET 2022 by jordi.olivares-provencio
