\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------------- MODULE ResumeTokenArming ----------------------------
\* Formal specification of the change-stream "lazy cursor arming" hazard.
\*
\* MOTIVATION
\* ----------
\* When a consumer opens a change-stream cursor with `db.coll.watch(pipeline)`
\* and does NOT pass `startAtOperationTime`, the server's documented
\* contract says the cursor's start point is "the time the cursor was
\* opened on the server". Some drivers implement cursor open lazily:
\* `watch()` returns to the caller without performing a server round-trip,
\* and the cursor is only actually registered when the iterator issues
\* its first `getMore`. Under that implementation the server-side
\* cursor's "open time" is the time the first `getMore` arrives, NOT
\* the time `watch()` returned. Any change event with a cluster
\* timestamp in the open window
\*
\*     [client_watch_returned, first_getMore_arrives_at_server)
\*
\* is silently dropped (see SERVER tracker for the corresponding driver
\* report).
\*
\* The documented workaround is to first call `db.command({hello: 1})`,
\* capture `operationTime`, and pin the cursor with
\* `startAtOperationTime = operationTime`. The server then opens the
\* cursor at the captured timestamp, not at first `getMore`, so the
\* open window is empty.
\*
\* This spec models both modes in one structure (parameterized by
\* `UseStartAt`) and demonstrates by TLC model-checking that:
\*
\*     - With UseStartAt = TRUE,  NoEventLoss is an invariant.
\*     - With UseStartAt = FALSE, NoEventLoss is violated by a
\*       counterexample of length ~5 in which a WriterEmit fires
\*       between ConsumerOpenCursorWithoutStartAt and ConsumerGetMore.
\*
\* The companion `MCResumeTokenArming.tla` instantiates the spec with
\* small finite constants and toggles `UseStartAt` to obtain both
\* verdicts.
\*
\* RELATIONSHIP TO EXISTING SPECS
\* ------------------------------
\* The existing `Replication/RaftMongo` spec models the oplog as an
\* unordered fact and `Sharding/MoveRange` models chunk routing; neither
\* speaks to change-stream cursor lifecycle. This is the first spec in
\* the `ChangeStreams/` directory.
\*
\* VARIABLES / STATE
\* -----------------
\*   clock          monotonically advancing cluster time (Nat, capped by
\*                  MaxClock so TLC state space is finite)
\*   oplog          Seq([ts: Nat, idx: Nat]); the oplog is append-only.
\*                  Each entry is emitted by the WriterEmit action and
\*                  carries the clock value at emit time.
\*   hello_ts       Nat \cup {NULL}; cluster time captured by the most
\*                  recent ConsumerHello, or NULL if no hello issued.
\*   cursor         One of: "absent" (no cursor opened), "open_lazy"
\*                  (cursor opened without startAtOperationTime; will
\*                  arm at first getMore), "open_pinned" (cursor opened
\*                  with startAtOperationTime; armed at open time),
\*                  "closed".
\*   cursor_start   The cluster timestamp from which the cursor's
\*                  delivery window begins. Becomes meaningful only when
\*                  the cursor is armed.
\*   armed          TRUE once the cursor has actually been registered on
\*                  the server side and is willing to deliver events
\*                  with ts >= cursor_start.
\*   delivered      The set of oplog indices the consumer has observed
\*                  via getMore. Strictly grows.
\*
\* ACTIONS
\* -------
\*   Tick                                Advance clock (≤ MaxClock).
\*   WriterEmit                          Append one entry to oplog with
\*                                       ts = clock.
\*   ConsumerHello                       Capture clock into hello_ts.
\*   ConsumerOpenCursorWithoutStartAt    Open lazy cursor (not yet armed).
\*   ConsumerOpenCursorWithStartAt(t)    Open pinned cursor; armed at t.
\*   ConsumerGetMore                     If cursor is open_lazy, server
\*                                       arms it at the current clock (the
\*                                       bug surface). Then deliver every
\*                                       buffered oplog index with
\*                                       ts >= cursor_start that the
\*                                       consumer hasn't seen.
\*
\* INVARIANTS
\* ----------
\*   NoEventLoss
\*       Once the cursor has been opened and the consumer has issued at
\*       least one ConsumerGetMore, for every oplog entry whose ts lies
\*       in the closed-open window [hello_ts, cursor_arm_ts), that entry
\*       must be in `delivered`. ("hello_ts" stands in for the timestamp
\*       the consumer believes is its baseline. The bug is that the
\*       consumer's belief about cursor start is BEFORE the server's
\*       actual arm time when the cursor is lazy.)
\*
\*   The temporal property `EventuallyDelivered` says any emitted event
\*   in the cursor's delivery window is eventually delivered.
\*
\* The spec is deliberately small (~150 lines without comments) so a
\* kernel engineer can audit it against the driver wire protocol.

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    MaxClock,        \* upper bound on cluster time (Nat)
    MaxEvents,       \* upper bound on oplog length (Nat)
    UseStartAt       \* BOOLEAN: TRUE => consumer pins cursor with hello_ts

ASSUME MaxClock \in Nat /\ MaxClock > 0
ASSUME MaxEvents \in Nat /\ MaxEvents > 0
ASSUME UseStartAt \in BOOLEAN

NULL == 0   \* sentinel for "no hello captured yet" / "no cursor_start". We
            \* use a numeric sentinel rather than a model-value so we can
            \* still compare with `<` and `>=` without TLC complaining
            \* about heterogeneous types. clock starts at 1 in any state
            \* where it matters; ts=0 is reserved for NULL.

VARIABLES
    clock,
    oplog,
    hello_ts,
    cursor,
    cursor_start,
    armed,
    delivered

vars == <<clock, oplog, hello_ts, cursor, cursor_start, armed, delivered>>

----
\* Initial state.

Init ==
    /\ clock = 1
    /\ oplog = <<>>
    /\ hello_ts = NULL
    /\ cursor = "absent"
    /\ cursor_start = NULL
    /\ armed = FALSE
    /\ delivered = {}

----
\* Helpers.

\* Indices of oplog entries that the armed cursor SHOULD deliver
\* (clusterTime >= cursor_start AND not yet delivered).
DeliverableIndices ==
    { i \in 1..Len(oplog) :
        /\ armed
        /\ oplog[i].ts >= cursor_start
        /\ i \notin delivered }

----
\* ACTIONS

\* Cluster time advances. Bounded by MaxClock so the model is finite.
Tick ==
    /\ clock < MaxClock
    /\ clock' = clock + 1
    /\ UNCHANGED <<oplog, hello_ts, cursor, cursor_start, armed, delivered>>

\* The writer (any application doing inserts/updates/deletes) appends
\* one event to the oplog. The event's cluster timestamp is the current
\* clock. We bound the oplog by MaxEvents so TLC terminates.
WriterEmit ==
    /\ Len(oplog) < MaxEvents
    /\ LET entry == [ts |-> clock, idx |-> Len(oplog) + 1]
       IN oplog' = Append(oplog, entry)
    /\ UNCHANGED <<clock, hello_ts, cursor, cursor_start, armed, delivered>>

\* Consumer issues `db.command({hello: 1})` and captures `operationTime`.
\* This is the cluster's notion of "now"; the consumer uses it as its
\* belief about the cursor's start window. Idempotent: a fresh hello
\* overwrites the previous capture.
ConsumerHello ==
    /\ cursor = "absent"
    /\ hello_ts' = clock
    /\ UNCHANGED <<clock, oplog, cursor, cursor_start, armed, delivered>>

\* THE BUG: open the cursor without `startAtOperationTime`. Under a
\* lazy-arming driver, this does NOT round-trip to the server until the
\* first getMore. The cursor is "open" from the client's point of view
\* but UNARMED on the server side. Any WriterEmit between this action
\* and the first ConsumerGetMore is lost.
ConsumerOpenCursorWithoutStartAt ==
    /\ ~UseStartAt
    /\ cursor = "absent"
    /\ hello_ts # NULL          \* the consumer issued hello first; that's
                                 \* what makes its belief observable
    /\ cursor' = "open_lazy"
    /\ UNCHANGED <<clock, oplog, hello_ts, cursor_start, armed, delivered>>

\* THE FIX: open the cursor WITH `startAtOperationTime = hello_ts`. The
\* driver round-trips on watch() and the server registers the cursor at
\* the pinned timestamp. The cursor is armed at that moment.
ConsumerOpenCursorWithStartAt ==
    /\ UseStartAt
    /\ cursor = "absent"
    /\ hello_ts # NULL
    /\ cursor' = "open_pinned"
    /\ cursor_start' = hello_ts
    /\ armed' = TRUE
    /\ UNCHANGED <<clock, oplog, hello_ts, delivered>>

\* ConsumerGetMore. Two behaviors depending on cursor mode:
\*   open_lazy   => server arms the cursor NOW (at current clock). Any
\*                  oplog entry with ts in [hello_ts, clock) is silently
\*                  dropped. Then deliver everything with ts >= clock.
\*   open_pinned => cursor is already armed at hello_ts; deliver
\*                  everything in DeliverableIndices.
\* In both modes, the consumer absorbs whatever is currently deliverable
\* into `delivered`. We model the "deliver one event" granularity so
\* TLC can interleave multiple getMore rounds.
ConsumerGetMore ==
    /\ cursor \in {"open_lazy", "open_pinned"}
    /\ \/ /\ cursor = "open_lazy"
          /\ ~armed
          /\ cursor_start' = clock
          /\ armed' = TRUE
          /\ cursor' = "open_pinned"   \* once armed, semantics merge
          /\ UNCHANGED <<clock, oplog, hello_ts, delivered>>
       \/ /\ armed
          /\ DeliverableIndices # {}
          /\ \E i \in DeliverableIndices: delivered' = delivered \cup {i}
          /\ UNCHANGED <<clock, oplog, hello_ts, cursor, cursor_start, armed>>
       \/ /\ armed
          /\ DeliverableIndices = {}
          /\ UNCHANGED <<clock, oplog, hello_ts, cursor, cursor_start, armed, delivered>>

\* Consumer may close the cursor at any time. Mostly here to give the
\* model a terminal state for `EventuallyClosed` if a user wants it.
CloseCursor ==
    /\ cursor \in {"open_lazy", "open_pinned"}
    /\ cursor' = "closed"
    /\ UNCHANGED <<clock, oplog, hello_ts, cursor_start, armed, delivered>>

----
\* Transition relation.

Next ==
    \/ Tick
    \/ WriterEmit
    \/ ConsumerHello
    \/ ConsumerOpenCursorWithoutStartAt
    \/ ConsumerOpenCursorWithStartAt
    \/ ConsumerGetMore
    \/ CloseCursor

Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(WriterEmit)
    /\ WF_vars(ConsumerHello)
    /\ WF_vars(ConsumerOpenCursorWithoutStartAt)
    /\ WF_vars(ConsumerOpenCursorWithStartAt)
    /\ WF_vars(ConsumerGetMore)

----
\* Type invariant.

TypeOK ==
    /\ clock \in 0..MaxClock
    /\ oplog \in Seq([ts: 0..MaxClock, idx: 1..MaxEvents])
    /\ Len(oplog) <= MaxEvents
    /\ hello_ts \in 0..MaxClock
    /\ cursor \in {"absent", "open_lazy", "open_pinned", "closed"}
    /\ cursor_start \in 0..MaxClock
    /\ armed \in BOOLEAN
    /\ delivered \subseteq 1..MaxEvents

\* Sanity: oplog timestamps are non-decreasing (they're stamped with
\* clock, which only goes up).
OplogMonotone ==
    \A i, j \in 1..Len(oplog): i <= j => oplog[i].ts <= oplog[j].ts

----
\* The headline invariant.
\*
\* NoEventLoss
\*
\* The consumer's CONTRACT after a successful hello + watch is: every
\* event whose ts >= hello_ts will eventually be delivered (or, in this
\* finite model, IS delivered once the cursor has been armed AND drained).
\*
\* The bug is that, in the lazy mode, the cursor's actual start
\* (`cursor_start`) is set at first-getMore, which is strictly later
\* than `hello_ts`. So an event with ts in [hello_ts, cursor_start) is
\* observable in the oplog but will NEVER be delivered.
\*
\* We state the invariant as: for every oplog index i, if the cursor is
\* armed AND oplog[i].ts >= hello_ts AND there are no more deliverable
\* events (the cursor has drained everything it CAN deliver), then i is
\* in `delivered`. The lazy path violates this; the pinned path does
\* not.

CursorDrained == armed /\ DeliverableIndices = {}

NoEventLoss ==
    CursorDrained =>
        \A i \in 1..Len(oplog):
            (hello_ts # NULL /\ oplog[i].ts >= hello_ts) => i \in delivered

----
\* Temporal properties.

\* Every event in the cursor's delivery window is eventually delivered.
\* Stated against the pinned path; for the lazy path TLC produces a
\* counterexample.
EventuallyDelivered ==
    \A i \in 1..MaxEvents:
        [](
            (i <= Len(oplog) /\ armed /\ oplog[i].ts >= cursor_start)
            ~> (i \in delivered)
        )

\* Optional: cursor eventually closes (only meaningful if the user adds
\* fairness on CloseCursor; not declared as a PROPERTY by default).
EventuallyClosed == <>(cursor = "closed")

==================================================================================
