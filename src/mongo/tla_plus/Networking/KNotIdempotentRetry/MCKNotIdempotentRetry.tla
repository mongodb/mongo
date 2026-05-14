---- MODULE MCKNotIdempotentRetry ----
\* Model-check configuration helper for KNotIdempotentRetry.tla.
\* See KNotIdempotentRetry.tla for the specification.

EXTENDS KNotIdempotentRetry

\* Bound the state-space. With MaxRequests=2 and MaxRetries=2 TLC explores
\* every interleaving of two concurrent requests where each request can be
\* sent up to three times (initial + 2 retries). This is enough to expose
\* NoDoubleApplication violations on the buggy classifier (one
\* RemoteAppliesThenFails followed by a DispatcherRetries followed by a
\* second RemoteAppliesThenFails is a 4-step witness).
StateConstraint ==
    /\ nextReqId <= MaxRequests
    /\ \A r \in 1..nextReqId : requests[r].attempts <= MaxRetries + 1

====
