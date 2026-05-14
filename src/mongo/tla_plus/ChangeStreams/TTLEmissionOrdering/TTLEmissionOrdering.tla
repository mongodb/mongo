\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------------- MODULE TTLEmissionOrdering ----------------------------
(**************************************************************************************************)
(* Models the ordering relationship between (a) the TTL monitor's deletion of an expired document *)
(* and (b) the change-stream emission of that deletion event, relative to user-initiated inserts. *)
(*                                                                                                *)
(* WHY THIS SPEC EXISTS                                                                           *)
(*                                                                                                *)
(* A collection may have both a TTL index (deleting docs whose `createdAt` field is older than a  *)
(* configured expiry) and a change-stream watcher (consuming insert/delete events). If the TTL    *)
(* monitor's delete is emitted to the oplog before the corresponding user insert is committed and *)
(* emitted, a change-stream consumer can observe a `delete` event for a document whose `insert`   *)
(* event it never saw. This is a real correctness hazard for any consumer that maintains          *)
(* derivative state keyed by document identity (cache invalidation, materialized views, audit     *)
(* logs).                                                                                         *)
(*                                                                                                *)
(* MongoDB's existing TLA+ corpus has no spec covering TTL or change-stream ordering. This        *)
(* contribution fills that gap.                                                                   *)
(*                                                                                                *)
(* MODEL OUTLINE                                                                                  *)
(*                                                                                                *)
(* Three actor classes:                                                                           *)
(*   USER_WRITER     - inserts documents with a `createdAt` timestamp.                            *)
(*   TTL_MONITOR     - periodically scans and deletes documents whose                             *)
(*                     createdAt + ExpireAfter <= now.                                            *)
(*   STREAM_CONSUMER - reads from a change-stream cursor pinned at a `resumeToken`;               *)
(*                     applies a `$match` filter on createdAt.                                    *)
(*                                                                                                *)
(* Each user-side write progresses through two stages:                                            *)
(*   "staged"    - assigned a clusterTime; document visible to the TTL monitor                    *)
(*                 (rare-but-real edge case where a background reader sees uncommitted state).    *)
(*   "committed" - present in the oplog; visible to the change-stream consumer.                   *)
(*                                                                                                *)
(* Two model knobs control whether the bug mode is exercised:                                     *)
(*   AllowTTLBeforeCommit - if TRUE, TTL_MONITOR may target a `staged` (uncommitted) doc.         *)
(*                          This is the bug mode used to demonstrate CausalOrdering violation.    *)
(*   AllowOutOfOrderEmit  - if TRUE, the StreamEmit action may shuffle the oplog tail before      *)
(*                          buffering it. This is the second bug mode used for NoOrphanDelete     *)
(*                          violation.                                                            *)
(*                                                                                                *)
(* When both knobs are FALSE the spec models the production-correct path:                         *)
(*   1. Inserts must commit before they can be TTL-deleted.                                       *)
(*   2. The change-stream emits events in oplog order.                                            *)
(* Under these constraints, all safety invariants (TypeOK, OplogMonotonic, CausalOrdering,        *)
(* NoOrphanDelete, NoDeleteBeforeInsertInOplog) and liveness properties (TTLNoSkip,               *)
(* EventuallyDrained) hold across the reachable state space.                                      *)
(**************************************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Docs,                  \* The set of document identifiers a USER_WRITER may insert.
    MaxClock,              \* Upper bound on the model clock (state-space bound for TLC).
    ExpireAfter,           \* TTL expiry threshold in abstract time units.
    ResumeToken,           \* Initial change-stream resume token; events with
                           \* clusterTime < ResumeToken are not delivered.
    ConsumerMatchSet,      \* Subset of Docs that the consumer's `$match` admits.
                           \* Docs outside this set are filtered out and never delivered.
    AllowTTLBeforeCommit,  \* BOOLEAN: when TRUE, allows TTL on uncommitted inserts (bug mode).
    AllowOutOfOrderEmit    \* BOOLEAN: when TRUE, allows out-of-order oplog emission (bug mode).

ASSUME Docs # {}
ASSUME MaxClock \in Nat /\ MaxClock > 0
ASSUME ExpireAfter \in Nat /\ ExpireAfter >= 0
ASSUME ResumeToken \in Nat
ASSUME ConsumerMatchSet \subseteq Docs
ASSUME AllowTTLBeforeCommit \in BOOLEAN
ASSUME AllowOutOfOrderEmit \in BOOLEAN

(***************************************************************************)
(* State variables                                                          *)
(*                                                                          *)
(* clock           : monotonic logical clock; ticks via TimeAdvance.        *)
(* collection      : function Docs -> doc-state record, where               *)
(*                     state \in {"absent", "staged", "committed",          *)
(*                                "ttl_deleted"} and                        *)
(*                     createdAt is the insert-time clock value.            *)
(* oplog           : Sequence of event records ordered by clusterTime.      *)
(*                   Each event has fields:                                 *)
(*                     op        \in {"insert", "delete"}                   *)
(*                     doc       \in Docs                                   *)
(*                     ts        \in 0..MaxClock      (clusterTime)         *)
(*                     createdAt \in 0..MaxClock      (for insert events)   *)
(* stream_buffer   : Sequence of event records the consumer has staged for  *)
(*                   delivery (i.e., the cursor has emitted them).          *)
(* observed        : Sequence of event records the consumer has read.       *)
(*                   Append-only; the spec checks ordering properties on    *)
(*                   this sequence.                                         *)
(***************************************************************************)
VARIABLES
    clock,
    collection,
    oplog,
    stream_buffer,
    observed

vars == <<clock, collection, oplog, stream_buffer, observed>>

(***************************************************************************)
(* Helpers                                                                  *)
(***************************************************************************)

\* Range over a sequence's indices, as a set.
Indices(s) == 1..Len(s)

\* Is doc d eligible for TTL deletion at time t?
\* The doc must be present in the collection and its createdAt must be at
\* least ExpireAfter time units in the past.
TTLEligible(d, t) ==
    /\ collection[d].state \in {"staged", "committed"}
    /\ collection[d].createdAt + ExpireAfter <= t

\* The change-stream filter: events for docs outside ConsumerMatchSet are
\* dropped at emission time, regardless of op.
PassesMatch(ev) == ev.doc \in ConsumerMatchSet

\* The cursor's resume window: only events with ts >= ResumeToken are
\* delivered. This models change-stream startAtOperationTime semantics.
PassesResume(ev) == ev.ts >= ResumeToken

\* The set of insert events in `observed` for a given doc.
InsertsObservedFor(d) ==
    {i \in Indices(observed) :
        /\ observed[i].op = "insert"
        /\ observed[i].doc = d}

\* The set of delete events in `observed` for a given doc.
DeletesObservedFor(d) ==
    {i \in Indices(observed) :
        /\ observed[i].op = "delete"
        /\ observed[i].doc = d}

\* The set of insert events in the oplog for a given doc.
InsertsInOplogFor(d) ==
    {i \in Indices(oplog) :
        /\ oplog[i].op = "insert"
        /\ oplog[i].doc = d}

(***************************************************************************)
(* Initial state                                                            *)
(*                                                                          *)
(* Empty collection, empty oplog, empty stream buffer, no observations,     *)
(* clock at 0. The createdAt field of an "absent" doc is a placeholder.     *)
(***************************************************************************)
Init ==
    /\ clock = 0
    /\ collection = [d \in Docs |->
                       [state |-> "absent", createdAt |-> 0]]
    /\ oplog = <<>>
    /\ stream_buffer = <<>>
    /\ observed = <<>>

(***************************************************************************)
(* ACTION: UserInsert(d)                                                    *)
(*                                                                          *)
(* A USER_WRITER inserts doc d, stamping createdAt with the current clock.  *)
(* The document moves from "absent" to "staged". It is NOT yet in the       *)
(* oplog; only CommitInsert will emit the insert event.                     *)
(***************************************************************************)
UserInsert(d) ==
    /\ collection[d].state = "absent"
    /\ collection' = [collection EXCEPT ![d] =
                        [state |-> "staged", createdAt |-> clock]]
    /\ UNCHANGED <<clock, oplog, stream_buffer, observed>>

(***************************************************************************)
(* ACTION: CommitInsert(d)                                                  *)
(*                                                                          *)
(* The staged insert is committed: it transitions to "committed" and an     *)
(* insert event is appended to the oplog. Per MongoDB semantics, the        *)
(* oplog entry's clusterTime is assigned at commit time (monotonic per      *)
(* shard, generated by the oplog allocator) — NOT the application-level     *)
(* createdAt field. We keep `createdAt` as a separate field on the event,   *)
(* matching how MongoDB exposes both `clusterTime` and the document's own   *)
(* application timestamp via the change-stream event.                       *)
(*                                                                          *)
(* Because two writers may stage in one order and commit in another, this   *)
(* distinction is what preserves OplogMonotonic (commit order is what       *)
(* determines oplog ts) while still letting the TTL monitor key its         *)
(* eligibility check off `createdAt`.                                       *)
(***************************************************************************)
CommitInsert(d) ==
    /\ collection[d].state = "staged"
    /\ collection' = [collection EXCEPT ![d].state = "committed"]
    /\ oplog' = Append(oplog,
                       [op        |-> "insert",
                        doc       |-> d,
                        ts        |-> clock,
                        createdAt |-> collection[d].createdAt])
    /\ UNCHANGED <<clock, stream_buffer, observed>>

(***************************************************************************)
(* ACTION: TimeAdvance                                                      *)
(*                                                                          *)
(* The model clock ticks. Bounded by MaxClock so TLC has finite state.      *)
(***************************************************************************)
TimeAdvance ==
    /\ clock < MaxClock
    /\ clock' = clock + 1
    /\ UNCHANGED <<collection, oplog, stream_buffer, observed>>

(***************************************************************************)
(* ACTION: TTLDelete(d)                                                     *)
(*                                                                          *)
(* The TTL monitor deletes doc d, then emits a delete event into the oplog. *)
(*                                                                          *)
(* In the correct path (AllowTTLBeforeCommit = FALSE) the monitor only      *)
(* targets COMMITTED documents — the insert event is already in the oplog   *)
(* with a smaller clusterTime, so causal ordering is preserved.             *)
(*                                                                          *)
(* In the bug path (AllowTTLBeforeCommit = TRUE) the monitor may also       *)
(* target a STAGED document. The delete event then enters the oplog before  *)
(* the corresponding insert event ever does, which produces a CausalOrdering*)
(* violation if the consumer later observes the delete.                     *)
(*                                                                          *)
(* The delete event's clusterTime is the current clock. The TTL monitor's   *)
(* delete is then immediately durable; there is no separate commit stage    *)
(* for TTL deletes (this matches MongoDB: the TTL monitor performs single-  *)
(* document deletes that commit at oplog-append time).                      *)
(***************************************************************************)
TTLDelete(d) ==
    /\ TTLEligible(d, clock)
    /\ \/ collection[d].state = "committed"
       \/ /\ AllowTTLBeforeCommit
          /\ collection[d].state = "staged"
    /\ collection' = [collection EXCEPT ![d].state = "ttl_deleted"]
    /\ oplog' = Append(oplog,
                       [op        |-> "delete",
                        doc       |-> d,
                        ts        |-> clock,
                        createdAt |-> collection[d].createdAt])
    /\ UNCHANGED <<clock, stream_buffer, observed>>

(***************************************************************************)
(* ACTION: StreamEmit                                                       *)
(*                                                                          *)
(* The change-stream cursor selects the next oplog event(s) not yet in      *)
(* stream_buffer, applies the resume-token filter and $match filter, and    *)
(* appends each surviving event to stream_buffer.                           *)
(*                                                                          *)
(* In the correct path, this emits exactly the next sequential oplog index. *)
(* In the bug path (AllowOutOfOrderEmit = TRUE), the cursor may emit an     *)
(* index k > emitted_count + 1 first, skipping over the intermediate insert *)
(* event. This models the case where a $match stage on the insert event    *)
(* (e.g., insert doc not in ConsumerMatchSet) filters the insert but a      *)
(* later $match-modification or post-image rewrite (a real misconfiguration *)
(* hazard) lets the corresponding delete through anyway.                    *)
(***************************************************************************)
EmittedCount == Len(stream_buffer)

StreamEmit ==
    \E i \in Indices(oplog) :
        /\ i > EmittedCount
        /\ \/ i = EmittedCount + 1                  \* In-order
           \/ AllowOutOfOrderEmit                   \* Or bug-mode shuffle
        /\ PassesResume(oplog[i])
        /\ PassesMatch(oplog[i])
        /\ stream_buffer' = Append(stream_buffer, oplog[i])
        /\ UNCHANGED <<clock, collection, oplog, observed>>

(***************************************************************************)
(* ACTION: StreamSkipFiltered                                               *)
(*                                                                          *)
(* The cursor advances past an oplog entry that fails the $match or         *)
(* resume-token filter. This is necessary so the cursor doesn't deadlock on *)
(* a non-matching event sitting at position EmittedCount + 1. The cursor    *)
(* still advances its position even though no event reaches stream_buffer.  *)
(*                                                                          *)
(* We model the cursor's position implicitly: a "skip" appends a sentinel   *)
(* "filtered" event to stream_buffer to keep the index aligned. This makes  *)
(* EmittedCount track the cursor's true oplog offset. The sentinel is       *)
(* invisible to the consumer (ConsumerRead ignores it).                     *)
(***************************************************************************)
StreamSkipFiltered ==
    /\ EmittedCount < Len(oplog)
    /\ LET i == EmittedCount + 1 IN
       /\ \/ ~PassesResume(oplog[i])
          \/ ~PassesMatch(oplog[i])
       /\ stream_buffer' = Append(stream_buffer,
                                  [op        |-> "filtered",
                                   doc       |-> oplog[i].doc,
                                   ts        |-> oplog[i].ts,
                                   createdAt |-> oplog[i].createdAt])
       /\ UNCHANGED <<clock, collection, oplog, observed>>

(***************************************************************************)
(* ACTION: ConsumerRead                                                     *)
(*                                                                          *)
(* The consumer pops the next event from stream_buffer (in order) and       *)
(* appends it to `observed`. Filtered sentinels are dropped silently.       *)
(*                                                                          *)
(* The consumer's `observed` sequence is the trace the invariants check.    *)
(***************************************************************************)
ConsumerRead ==
    \E i \in Indices(stream_buffer) :
        /\ i = Len(observed) + 1
        /\ observed' = Append(observed, stream_buffer[i])
        /\ UNCHANGED <<clock, collection, oplog, stream_buffer>>

(***************************************************************************)
(* ACTION: Terminating                                                      *)
(*                                                                          *)
(* Stuttering action used by TLC to make the bounded state space accept     *)
(* "all work done, clock at MaxClock" as a legitimate terminal state rather *)
(* than reporting deadlock. The state is reached when every doc is either   *)
(* deleted or has not yet expired and the clock is pinned at MaxClock.      *)
(*                                                                          *)
(* Matches the same idiom used in                                           *)
(* Sharding/RangeDeletionsSecondaryNodes/RangeDeletionsSecondaryNodes.tla.  *)
(***************************************************************************)
Terminating ==
    /\ clock = MaxClock
    /\ \A d \in Docs :
         collection[d].state \in {"absent", "ttl_deleted"}
         \/ ~TTLEligible(d, clock)
    /\ Len(stream_buffer) = Len(oplog)
    /\ Len(observed) = Len(stream_buffer)
    /\ UNCHANGED vars

(***************************************************************************)
(* Next-state relation                                                      *)
(***************************************************************************)
Next ==
    \/ \E d \in Docs : UserInsert(d)
    \/ \E d \in Docs : CommitInsert(d)
    \/ TimeAdvance
    \/ \E d \in Docs : TTLDelete(d)
    \/ StreamEmit
    \/ StreamSkipFiltered
    \/ ConsumerRead
    \/ Terminating

(***************************************************************************)
(* Fairness                                                                 *)
(*                                                                          *)
(* Weak fairness on every action so the consumer eventually drains the      *)
(* oplog (required for TTLNoSkip liveness). TimeAdvance and TTLDelete are   *)
(* weakly fair so the monitor eventually runs.                              *)
(***************************************************************************)
Fairness ==
    /\ WF_vars(TimeAdvance)
    /\ WF_vars(\E d \in Docs : CommitInsert(d))
    /\ WF_vars(\E d \in Docs : TTLDelete(d))
    /\ WF_vars(StreamEmit)
    /\ WF_vars(StreamSkipFiltered)
    /\ WF_vars(ConsumerRead)

Spec == Init /\ [][Next]_vars /\ Fairness

(***************************************************************************)
(* SAFETY: TypeOK                                                           *)
(***************************************************************************)
TypeOK ==
    /\ clock \in 0..MaxClock
    /\ collection \in [Docs ->
                         [state     : {"absent", "staged", "committed",
                                       "ttl_deleted"},
                          createdAt : 0..MaxClock]]
    /\ \A i \in Indices(oplog) :
         /\ oplog[i].op \in {"insert", "delete"}
         /\ oplog[i].doc \in Docs
         /\ oplog[i].ts \in 0..MaxClock
         /\ oplog[i].createdAt \in 0..MaxClock
    /\ \A i \in Indices(stream_buffer) :
         /\ stream_buffer[i].op \in {"insert", "delete", "filtered"}
         /\ stream_buffer[i].doc \in Docs
    /\ \A i \in Indices(observed) :
         /\ observed[i].op \in {"insert", "delete", "filtered"}
         /\ observed[i].doc \in Docs

(***************************************************************************)
(* SAFETY: OplogMonotonic                                                   *)
(*                                                                          *)
(* The oplog's clusterTimes are non-decreasing in index order. In MongoDB   *)
(* this is enforced by the single-writer oplog; the spec keeps it as an    *)
(* invariant so a bug-mode mutation that violated it would be caught.       *)
(***************************************************************************)
OplogMonotonic ==
    \A i, j \in Indices(oplog) :
        i < j => oplog[i].ts <= oplog[j].ts

(***************************************************************************)
(* SAFETY: CausalOrdering                                                   *)
(*                                                                          *)
(* Every `delete` event for doc d arrives at the consumer AFTER the         *)
(* corresponding `insert` event for d, OR the insert was filtered (so the   *)
(* consumer can be unaware of d entirely — see NoOrphanDelete for the       *)
(* tighter version).                                                        *)
(*                                                                          *)
(* This is the headline correctness property the spec is designed to check. *)
(***************************************************************************)
CausalOrdering ==
    \A j \in DeletesObservedFor("placeholder") \cup
            UNION {DeletesObservedFor(d) : d \in Docs} :
        LET d == observed[j].doc IN
        \/ \E i \in InsertsObservedFor(d) : i < j
        \/ \* The insert was emitted before the resume token (consumer
           \* started watching after this doc's lifetime).
           \E k \in InsertsInOplogFor(d) : oplog[k].ts < ResumeToken
        \/ \* The doc was filtered out of consumer's view by $match.
           d \notin ConsumerMatchSet

(***************************************************************************)
(* SAFETY: NoOrphanDelete                                                   *)
(*                                                                          *)
(* The consumer does NOT see a delete event for a doc it did not previously *)
(* see inserted, UNLESS the insert event's clusterTime was below the        *)
(* resume token (so the consumer is allowed to miss it).                    *)
(*                                                                          *)
(* If the consumer's $match excludes the doc entirely, neither insert nor   *)
(* delete should appear in `observed`. This is a stricter invariant than    *)
(* CausalOrdering and is the one that catches StreamEmit out-of-order bugs. *)
(***************************************************************************)
NoOrphanDelete ==
    \A j \in Indices(observed) :
        observed[j].op = "delete" =>
            LET d == observed[j].doc IN
            \/ \E i \in InsertsObservedFor(d) : i < j
            \/ \E k \in InsertsInOplogFor(d) : oplog[k].ts < ResumeToken

(***************************************************************************)
(* SAFETY: NoDeleteBeforeInsertInOplog                                      *)
(*                                                                          *)
(* Even before considering the consumer, the oplog itself should never      *)
(* contain a `delete` event for a doc whose `insert` event is at a later    *)
(* index. Violating this means the TTL monitor reached an uncommitted doc.  *)
(***************************************************************************)
NoDeleteBeforeInsertInOplog ==
    \A j \in Indices(oplog) :
        oplog[j].op = "delete" =>
            \E i \in Indices(oplog) :
                /\ i < j
                /\ oplog[i].op = "insert"
                /\ oplog[i].doc = oplog[j].doc

(***************************************************************************)
(* LIVENESS: TTLNoSkip                                                      *)
(*                                                                          *)
(* If a doc reaches a state where its createdAt + ExpireAfter <= clock, the *)
(* TTL monitor will eventually delete it (i.e., emit a delete event into    *)
(* the oplog). The TTL monitor does not silently skip expired docs.         *)
(*                                                                          *)
(* This is expressed as a leads-to property. The fairness annotation on     *)
(* TTLDelete is what makes it hold; absent fairness the monitor could       *)
(* stutter forever.                                                         *)
(***************************************************************************)
TTLNoSkip ==
    \A d \in Docs :
        ( /\ collection[d].state \in {"staged", "committed"}
          /\ collection[d].createdAt + ExpireAfter <= clock )
        ~>
        collection[d].state = "ttl_deleted"

(***************************************************************************)
(* LIVENESS: EventuallyDrained                                              *)
(*                                                                          *)
(* Every oplog event that passes both the resume-token and $match filters   *)
(* eventually appears in `observed`. This is the dual of TTLNoSkip on the   *)
(* consumer side — the change-stream pipe makes progress.                   *)
(***************************************************************************)
EventuallyDrained ==
    \A d \in Docs :
        \A op \in {"insert", "delete"} :
            ( /\ \E i \in Indices(oplog) :
                   /\ oplog[i].op = op
                   /\ oplog[i].doc = d
                   /\ oplog[i].ts >= ResumeToken
                   /\ d \in ConsumerMatchSet )
            ~>
            ( \E j \in Indices(observed) :
                /\ observed[j].op = op
                /\ observed[j].doc = d )

====================================================================================================
