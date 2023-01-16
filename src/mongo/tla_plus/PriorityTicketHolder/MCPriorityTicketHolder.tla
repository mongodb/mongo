---- MODULE MCPriorityTicketHolder ----

EXTENDS PriorityTicketHolder

TicketsAreAtMostTheInitialNumber ==
ticketsAvailable <= InitialNumberOfTicketsAvailable /\ ticketsAvailable >= 0

AllTicketsAvailableImpliesNoWaiters ==
ticketsAvailable = InitialNumberOfTicketsAvailable => (numQueuedProc[1] + numQueuedProc[2] = 0)

NumQueuedAlwaysGreaterOrEqualTo0 ==
    numQueuedProc[1] >= 0 /\ numQueuedProc[2] >= 0
=============================================================================
