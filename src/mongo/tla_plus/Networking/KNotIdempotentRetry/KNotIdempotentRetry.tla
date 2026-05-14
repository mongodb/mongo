\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---- MODULE KNotIdempotentRetry ----
\* Models the Shard::RetryPolicy::kNotIdempotent classifier in
\* src/mongo/db/sharding_environment/client/shard.cpp, with the goal of
\* disentangling which NotPrimary error subclasses are safe to retry under a
\* non-idempotent write (or non-idempotent non-write, e.g. getMore).
\*
\* Motivation (SERVER-108482):
\*   The current classifier returns true for the entire ErrorCodes::isNotPrimaryError
\*   category. The error_codes.yml comment explicitly states that the category by
\*   itself is INSUFFICIENT to determine whether a write actually occurred -- each
\*   member code must be inspected individually.
\*
\*   PR #52940 attempted to remove the whole category from the kNotIdempotent retry
\*   set and was reverted (#53185) because the all-or-nothing change broke tests
\*   that relied on retrying NotWritablePrimary after retargeting (the
\*   "rejected-before-apply" case is genuinely safe and is the load-bearing
\*   reason kNotIdempotent retries NotPrimary at all).
\*
\* Modelling strategy:
\*   Three orthogonal axes per request:
\*     1. RequestState  in  {PreSend, SentUnknown, Applied, NotApplied}
\*        -- where the request is in its life-cycle from the client's POV.
\*     2. NotPrimarySub in  {NotWritablePrimary, NotPrimaryNoSecondaryOk,
\*                           PrimarySteppedDown, InterruptedDueToReplStateChange,
\*                           NoNotPrimary}
\*        -- the precise error subclass returned to the client.
\*     3. IdemClass     in  {NonIdempotentWrite, NonIdempotentNonWrite, Idempotent}
\*        -- the operation's idempotency class as declared by the dispatcher.
\*
\*   The safety invariant NoDoubleApplication asserts that across any history
\*   the same logical operation is Applied on the primary at most once even
\*   when the retry classifier fires.
\*
\* Bug toggle:
\*   The boolean constant AllowRetryOnAmbiguousNotPrimary models the pre-revert
\*   (= currently-live) behaviour: when TRUE the classifier retries on the FULL
\*   NotPrimary category, including PrimarySteppedDown and
\*   InterruptedDueToReplStateChange. When FALSE the classifier retries on the
\*   minimum-safe subset only -- NotWritablePrimary and NotPrimaryNoSecondaryOk
\*   (i.e. the rejected-before-apply subset).
\*
\* This spec is deliberately scoped: it does not re-derive RaftMongo's election
\* / commit-point logic. It models the single client request crossing a single
\* primary boundary, because that is exactly the seam where the SERVER-108482
\* bug lives.

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    MaxRequests,                          \* bound on Requests issued (model-check finite)
    MaxRetries,                           \* bound on retry depth per request
    AllowRetryOnAmbiguousNotPrimary       \* the bug toggle (TRUE = current code)

ASSUME MaxRequests \in Nat /\ MaxRequests >= 1
ASSUME MaxRetries  \in Nat /\ MaxRetries  >= 1
ASSUME AllowRetryOnAmbiguousNotPrimary \in BOOLEAN

\* --- Enumerations as model values ---------------------------------------

RequestStates ==
    {"PreSend",        \* dispatcher holds the request, not yet on the wire
     "SentUnknown",    \* on the wire, outcome ambiguous until reply lands
     "Applied",        \* primary applied the op (write committed locally or replicated)
     "NotApplied"}     \* primary rejected the op before any side effect

NotPrimarySubs ==
    {"NotWritablePrimary",                 \* server rejected: was not writable primary
     "NotPrimaryNoSecondaryOk",            \* server rejected: secondary read w/o secondaryOk
     "PrimarySteppedDown",                 \* primary lost leadership mid-flight
     "InterruptedDueToReplStateChange",    \* op interrupted by repl state transition
     "NoNotPrimary"}                       \* a non-NotPrimary reply

IdemClasses ==
    {"NonIdempotentWrite",                 \* e.g. user-management write, $merge w/ user-set wc
     "NonIdempotentNonWrite",              \* e.g. getMore on a cursor with side effects
     "Idempotent"}                         \* covered by kIdempotent, included for completeness

\* The rejected-before-apply subset. error_codes.yml's NotPrimaryError category
\* mixes two semantics: codes that *guarantee* the server never reached the apply
\* path (NotWritablePrimary, NotPrimaryNoSecondaryOk) and codes that *do not*
\* (PrimarySteppedDown, InterruptedDueToReplStateChange). The minimum-safe subset
\* is the first group only.
RejectedBeforeApplySubset ==
    {"NotWritablePrimary", "NotPrimaryNoSecondaryOk"}

\* The ambiguous subset: server may or may not have applied; the client cannot tell
\* without a NoWritesPerformed label (SERVER-66479).
AmbiguousNotPrimarySubset ==
    {"PrimarySteppedDown", "InterruptedDueToReplStateChange"}

\* --- State ---------------------------------------------------------------

VARIABLES
    requests,        \* function: requestId -> request record
    appliedCount,    \* function: requestId -> number of times op was Applied on primary
    nextReqId        \* monotonic counter (bounded by MaxRequests)

vars == <<requests, appliedCount, nextReqId>>

RequestId == 1..MaxRequests

\* Shape of a request record. attempts is the count of network sends for this op.
\* state, lastError track the most recent outcome the client has observed.
RequestRecord ==
    [idem        : IdemClasses,
     state       : RequestStates,
     lastError   : NotPrimarySubs,
     attempts    : 0..(MaxRetries + 1),
     terminated  : BOOLEAN]

TypeOK ==
    /\ nextReqId \in 0..MaxRequests
    /\ DOMAIN requests = 1..nextReqId
    /\ DOMAIN appliedCount = 1..nextReqId
    /\ \A r \in 1..nextReqId :
        /\ requests[r] \in RequestRecord
        /\ appliedCount[r] \in 0..(MaxRetries + 1)

\* --- Initial state -------------------------------------------------------

Init ==
    /\ nextReqId    = 0
    /\ requests     = <<>>
    /\ appliedCount = <<>>

\* --- Helpers -------------------------------------------------------------

\* The retry classifier under test. Returns TRUE iff kNotIdempotent retries
\* given the observed lastError. NoNotPrimary errors (timeouts, network errors,
\* etc.) are out of scope for this spec; we model only the NotPrimary axis.
ClassifierRetries(err) ==
    CASE err = "NoNotPrimary"
            -> FALSE
      [] err \in RejectedBeforeApplySubset
            -> TRUE                                  \* always safe
      [] err \in AmbiguousNotPrimarySubset
            -> AllowRetryOnAmbiguousNotPrimary       \* bug toggle
      [] OTHER
            -> FALSE

\* Has the request reached a terminal state (success, fatal error, or retry-budget
\* exhausted)?
IsTerminal(rec) ==
    \/ rec.terminated
    \/ rec.attempts >= MaxRetries + 1

\* --- Actions -------------------------------------------------------------

\* The dispatcher issues a new request and immediately transitions it to
\* SentUnknown (we collapse PreSend -> SentUnknown into one TLC step because
\* the bug does not depend on what happens before the wire send).
IssueRequest ==
    /\ nextReqId < MaxRequests
    /\ \E ic \in IdemClasses :
        LET newId == nextReqId + 1
            rec   == [idem       |-> ic,
                      state      |-> "SentUnknown",
                      lastError  |-> "NoNotPrimary",
                      attempts   |-> 1,
                      terminated |-> FALSE]
        IN /\ nextReqId'    = newId
           /\ requests'     = requests     @@ (newId :> rec)
           /\ appliedCount' = appliedCount @@ (newId :> 0)

\* The primary RECEIVED the request, REJECTED it before apply, and replied with
\* a rejected-before-apply NotPrimary subclass. appliedCount is NOT incremented.
\* Note: we deliberately do not gate Remote* on ~IsTerminal because once a
\* request has been put on the wire the remote can always answer; the retry
\* budget bounds retries, not in-flight responses.
RemoteRejectsBeforeApply(r) ==
    /\ r \in 1..nextReqId
    /\ requests[r].state = "SentUnknown"
    /\ ~requests[r].terminated
    /\ \E sub \in RejectedBeforeApplySubset :
        requests' = [requests EXCEPT ![r] =
            [@ EXCEPT !.state = "NotApplied", !.lastError = sub]]
    /\ UNCHANGED <<appliedCount, nextReqId>>

\* The primary APPLIED the op and THEN failed (e.g. stepped down between apply
\* and reply ack). This is the ambiguous case: from the client's POV the reply
\* indicates a NotPrimary subclass that does not preclude apply.
RemoteAppliesThenFails(r) ==
    /\ r \in 1..nextReqId
    /\ requests[r].state = "SentUnknown"
    /\ ~requests[r].terminated
    /\ \E sub \in AmbiguousNotPrimarySubset :
        /\ requests' = [requests EXCEPT ![r] =
                [@ EXCEPT !.state = "Applied", !.lastError = sub]]
        /\ appliedCount' = [appliedCount EXCEPT ![r] = @ + 1]
    /\ UNCHANGED <<nextReqId>>

\* The primary APPLIED the op and replied success. Idempotency does not matter
\* here -- the request terminates with state Applied and the classifier never
\* fires. Kept so behaviours include the success path.
RemoteAppliesAndAcks(r) ==
    /\ r \in 1..nextReqId
    /\ requests[r].state = "SentUnknown"
    /\ ~requests[r].terminated
    /\ requests' = [requests EXCEPT ![r] =
            [@ EXCEPT !.state = "Applied", !.terminated = TRUE]]
    /\ appliedCount' = [appliedCount EXCEPT ![r] = @ + 1]
    /\ UNCHANGED <<nextReqId>>

\* The dispatcher inspects the observed error and decides to retry.
\* This is the action the classifier under test gates.
\* When the request is re-sent we go back to SentUnknown so the primary can
\* RemoteApplies* again -- if appliedCount climbs to 2, NoDoubleApplication
\* fails.
DispatcherRetries(r) ==
    /\ r \in 1..nextReqId
    /\ ~requests[r].terminated
    /\ requests[r].state \in {"Applied", "NotApplied"}
    /\ ClassifierRetries(requests[r].lastError)
    /\ requests[r].idem \in {"NonIdempotentWrite", "NonIdempotentNonWrite"}
    /\ requests[r].attempts < MaxRetries + 1
    /\ requests' = [requests EXCEPT ![r] =
            [@ EXCEPT !.state    = "SentUnknown",
                      !.attempts = @ + 1]]
    /\ UNCHANGED <<appliedCount, nextReqId>>

\* The dispatcher surfaces the error to the caller (gives up retrying).
\* Four reasons to give up:
\*   1. The classifier says "no retry" for the observed error.
\*   2. The retry budget is exhausted.
\*   3. The request is Idempotent -- model scope is kNotIdempotent only, so an
\*      Idempotent request that lands here simply terminates without modelling
\*      kIdempotent's separate retry path.
\*   4. The request received a non-NotPrimary reply that nonetheless landed
\*      in NotApplied (catch-all so every reply path terminates the request).
DispatcherGivesUp(r) ==
    /\ r \in 1..nextReqId
    /\ ~requests[r].terminated
    /\ requests[r].state \in {"Applied", "NotApplied"}
    /\ \/ ~ClassifierRetries(requests[r].lastError)
       \/ requests[r].attempts >= MaxRetries + 1
       \/ requests[r].idem = "Idempotent"
    /\ requests' = [requests EXCEPT ![r] = [@ EXCEPT !.terminated = TRUE]]
    /\ UNCHANGED <<appliedCount, nextReqId>>

\* Self-stutter step: once every issued request has terminated and the budget
\* of new requests is exhausted, there is nothing left to do. Without an
\* explicit terminal stutter TLC reports the natural end-state as a deadlock
\* error -- which is noise for a spec that legitimately has a finite life-cycle.
Done ==
    /\ nextReqId = MaxRequests
    /\ \A r \in 1..nextReqId : requests[r].terminated
    /\ UNCHANGED vars

Next ==
    \/ IssueRequest
    \/ \E r \in 1..nextReqId :
            \/ RemoteRejectsBeforeApply(r)
            \/ RemoteAppliesThenFails(r)
            \/ RemoteAppliesAndAcks(r)
            \/ DispatcherRetries(r)
            \/ DispatcherGivesUp(r)
    \/ Done

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

\* --- Safety invariants ---------------------------------------------------

\* The headline invariant: for any non-idempotent operation the primary must
\* apply the op at most once across the entire retry history.
NoDoubleApplication ==
    \A r \in 1..nextReqId :
        requests[r].idem \in {"NonIdempotentWrite", "NonIdempotentNonWrite"} =>
            appliedCount[r] <= 1

\* Sanity invariant: a request cannot exceed its retry budget.
RetryBudgetRespected ==
    \A r \in 1..nextReqId : requests[r].attempts <= MaxRetries + 1

\* --- Liveness ------------------------------------------------------------

\* Every request eventually terminates. This is a weak liveness property to
\* make sure the dispatcher doesn't get stuck in an infinite retry loop on the
\* same error -- which the MaxRetries budget already prevents structurally.
AllRequestsEventuallyTerminate ==
    \A r \in 1..MaxRequests :
        (r <= nextReqId) ~> requests[r].terminated

====
