---- MODULE MCPriorityTicketHolder ----

EXTENDS PriorityTicketHolder

TicketsAreAtMostTheInitialNumber ==
ticketsAvailable <= InitialNumberOfTicketsAvailable /\ ticketsAvailable >= 0

AllTicketsAvailableImpliesNoWaiters ==
ticketsAvailable = InitialNumberOfTicketsAvailable => (queuedProcs = {})
=============================================================================
