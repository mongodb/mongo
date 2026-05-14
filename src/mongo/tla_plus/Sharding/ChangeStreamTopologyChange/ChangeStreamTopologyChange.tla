\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------------- MODULE ChangeStreamTopologyChange ----------------------------
\*
\* This specification models the change-stream V2 shard-targeter and topology-change state
\* machine on a sharded cluster. It covers the interleaving of:
\*
\*   - addShard / removeShard issued by the Coordinator,
\*   - per-shard oplog appends (writes),
\*   - a Consumer that opens the stream with a startAtOperationTime (a resume token's
\*     clusterTime) and issues getMore against per-shard cursors,
\*   - per-shard primary stepdown, which closes and reopens cursors but preserves the
\*     consumer-side resume token.
\*
\* The pipeline being modelled is the topology-handler stage that watches for new shards
\* mid-stream and opens a cursor on each new shard at an appropriate clusterTime. The
\* canonical reference implementation is
\*   src/mongo/db/exec/agg/change_stream_handle_topology_change_stage.cpp
\* and the helper
\*   src/mongo/db/pipeline/change_stream_topology_helpers.cpp
\*
\* The spec deliberately abstracts:
\*   - resume-token encoding (modeled as the clusterTime numeric, no UUID/eventId),
\*   - $mergeCursors merge-ordering (modeled as a per-shard cursor + a global HWM that the
\*     consumer advances post-batch),
\*   - replication / oplog hole-filling (writes commit instantaneously).
\*
\* Two safety invariants and one liveness property:
\*   - NoEventBeforeResumeToken (safety) — the consumer never delivers an event whose
\*     clusterTime is strictly less than its resume token. This is the core property
\*     violated by SERVER-48386 and SERVER-124540 (pre-fix), where the new-shard cursor
\*     was opened at shardAddedTime+1 even when shardAddedTime < startAtOperationTime,
\*     causing pre-future events to leak past the resume token.
\*   - MonotonicHighWatermark (safety) — the consumer's high-water-mark resume token
\*     is non-decreasing across getMore calls and across topology changes.
\*   - EventuallyDelivered (liveness) — every committed event with clusterTime greater
\*     than the consumer's resume token, on an active shard, is eventually delivered to
\*     the consumer (modulo stepdowns, which only re-establish cursors).
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Sharding/ChangeStreamTopologyChange
\*

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    ShardIds,                          \* universe of possible shard ids
    MaxClusterTime,                    \* upper bound on coordinator clusterTime
    MaxWrites,                         \* upper bound on total oplog appends across all shards
    MaxTopologyChanges,                \* upper bound on addShard+removeShard count
    InitialActiveShards,               \* shards that are active in the initial cluster state
    ConsumerStartAtOperationTime,      \* the resume-token clusterTime the consumer opens at
    AllowNewShardCursorBelowResumeToken  \* TRUE => buggy pre-SERVER-48386 formula

ASSUME Cardinality(ShardIds) >= 2
ASSUME MaxClusterTime \in Nat /\ MaxClusterTime >= 1
ASSUME MaxWrites \in Nat
ASSUME MaxTopologyChanges \in Nat
ASSUME InitialActiveShards \subseteq ShardIds /\ Cardinality(InitialActiveShards) >= 1
ASSUME ConsumerStartAtOperationTime \in Nat
ASSUME AllowNewShardCursorBelowResumeToken \in BOOLEAN

UninitializedTs == 0
NoEvent == [shard |-> "-", ts |-> UninitializedTs]

(* Coordinator state *)
VARIABLE coordinatorTime    \* monotonic global clusterTime tick
VARIABLE activeShards       \* set of shards currently in the cluster
VARIABLE shardAddedAt       \* function: shard -> clusterTime at which the shard joined
VARIABLE topologyChanges    \* counter: total addShard+removeShard calls

(* Per-shard oplog state *)
VARIABLE oplog              \* function: shard -> sequence of [ts: Nat]
VARIABLE writes             \* counter: total appended oplog entries

(* Per-shard replica-set state (only the primary serves the change stream cursor) *)
VARIABLE shardPrimaryEpoch  \* function: shard -> Nat (bumped on every stepdown)

(* Consumer state *)
VARIABLE consumerResumeToken \* clusterTime: the startAtOperationTime the stream opened at
VARIABLE consumerHWM         \* clusterTime: highest event clusterTime acknowledged by consumer
VARIABLE consumerDelivered   \* sequence of delivered events, each [shard, ts]
VARIABLE shardCursor         \* function: shard -> [openedAt: Nat, epoch: Nat, pos: Nat] |
                             \*                    NoCursor

NoCursor == [openedAt |-> UninitializedTs, epoch |-> 0, pos |-> 0]

coordinator_vars == <<coordinatorTime, activeShards, shardAddedAt, topologyChanges>>
oplog_vars       == <<oplog, writes>>
shard_vars       == <<shardPrimaryEpoch>>
consumer_vars    == <<consumerResumeToken, consumerHWM, consumerDelivered, shardCursor>>
vars             == <<coordinator_vars, oplog_vars, shard_vars, consumer_vars>>

Max(a, b) == IF a >= b THEN a ELSE b

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Initial state                                                                                  *)
(**************************************************************************************************)

\* The model opens the change stream with a startAtOperationTime that is in the FUTURE relative
\* to the initial coordinator clock. The SERVER-48386 race depends on this future-time gap:
\* AddShard fires while coordinatorTime < consumerResumeToken, producing a shardAddedTime that is
\* itself below the resume token.
Init ==
    /\ coordinatorTime = 1
    /\ activeShards    = InitialActiveShards
    /\ shardAddedAt    = [s \in ShardIds |->
                            IF s \in InitialActiveShards
                                THEN 1
                                ELSE UninitializedTs]
    /\ topologyChanges = 0
    /\ oplog           = [s \in ShardIds |-> <<>>]
    /\ writes          = 0
    /\ shardPrimaryEpoch = [s \in ShardIds |-> 1]
    /\ consumerResumeToken = ConsumerStartAtOperationTime
    /\ consumerHWM         = ConsumerStartAtOperationTime
    /\ consumerDelivered   = <<>>
    \* The consumer opens cursors on each initial shard at the startAtOperationTime (a future
    \* time). The cursors remain open but yield nothing until oplog entries land at >= that time.
    /\ shardCursor = [s \in ShardIds |->
                        IF s \in InitialActiveShards
                            THEN [openedAt |-> ConsumerStartAtOperationTime,
                                  epoch    |-> 1,
                                  pos      |-> 0]
                            ELSE NoCursor]

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Actions                                                                                        *)
(**************************************************************************************************)

\* Action: the coordinator ticks the global cluster time. Models passage of time independently of
\* writes, used to make the "shard added at a time when no writes occurred there" race observable.
Tick ==
    /\ coordinatorTime < MaxClusterTime
    /\ coordinatorTime' = coordinatorTime + 1
    /\ UNCHANGED <<activeShards, shardAddedAt, topologyChanges,
                   oplog_vars, shard_vars, consumer_vars>>

\* Action: append an oplog entry on an active shard at the current coordinator time.
WriteOnShard(s) ==
    /\ writes < MaxWrites
    /\ s \in activeShards
    /\ coordinatorTime < MaxClusterTime
    /\ coordinatorTime' = coordinatorTime + 1
    /\ oplog' = [oplog EXCEPT ![s] = Append(@, [ts |-> coordinatorTime'])]
    /\ writes' = writes + 1
    /\ UNCHANGED <<activeShards, shardAddedAt, topologyChanges,
                   shard_vars, consumer_vars>>

\* Action: coordinator adds a new shard. The new-shard event becomes visible to the consumer's
\* topology-handler stage, which opens a cursor on the new shard at cursorStartTime.
\*
\* The buggy (pre-SERVER-48386) formula sets cursorStartTime := shardAddedTime + 1, which can be
\* strictly less than consumerResumeToken when consumerResumeToken is a future-time.
\*
\* The fixed formula sets cursorStartTime := max(shardAddedTime + 1, consumerResumeToken) so
\* that the new-shard cursor never opens below the original resume token (SERVER-124540 made
\* this max use the original resume token directly rather than the HWM, which can lag).
AddShard(s) ==
    /\ topologyChanges < MaxTopologyChanges
    /\ s \in ShardIds \ activeShards
    /\ coordinatorTime < MaxClusterTime
    /\ coordinatorTime' = coordinatorTime + 1
    /\ activeShards'    = activeShards \cup {s}
    /\ shardAddedAt'    = [shardAddedAt EXCEPT ![s] = coordinatorTime']
    /\ topologyChanges' = topologyChanges + 1
    /\ LET shardAddedTimePlusOne == coordinatorTime' + 1
           buggyStartTs          == shardAddedTimePlusOne
           fixedStartTs          == Max(shardAddedTimePlusOne, consumerResumeToken)
           cursorStartTime       == IF AllowNewShardCursorBelowResumeToken
                                        THEN buggyStartTs
                                        ELSE fixedStartTs IN
        shardCursor' = [shardCursor EXCEPT ![s] = [openedAt |-> cursorStartTime,
                                                   epoch    |-> shardPrimaryEpoch[s],
                                                   pos      |-> 0]]
    /\ UNCHANGED <<oplog_vars, shard_vars,
                   consumerResumeToken, consumerHWM, consumerDelivered>>

\* Action: coordinator removes a shard. The consumer's cursor on that shard is dropped.
RemoveShard(s) ==
    /\ topologyChanges < MaxTopologyChanges
    /\ s \in activeShards
    /\ Cardinality(activeShards) > 1
    /\ coordinatorTime < MaxClusterTime
    /\ coordinatorTime' = coordinatorTime + 1
    /\ activeShards'    = activeShards \ {s}
    /\ topologyChanges' = topologyChanges + 1
    /\ shardCursor' = [shardCursor EXCEPT ![s] = NoCursor]
    /\ UNCHANGED <<shardAddedAt, oplog_vars, shard_vars,
                   consumerResumeToken, consumerHWM, consumerDelivered>>

\* Action: stepdown on a shard's primary. The replica-set epoch bumps, the cursor on that shard
\* is closed, then re-established at the consumer's high-water-mark (the resume-after path).
\*
\* This re-establishment must use a time that does not regress below the resume token, even if
\* the HWM has not advanced since the stream opened.
Stepdown(s) ==
    /\ s \in activeShards
    /\ topologyChanges < MaxTopologyChanges
    /\ shardPrimaryEpoch' = [shardPrimaryEpoch EXCEPT ![s] = @ + 1]
    /\ topologyChanges' = topologyChanges + 1
    /\ LET reopenTs == Max(consumerHWM, consumerResumeToken) IN
        shardCursor' = [shardCursor EXCEPT ![s] = [openedAt |-> reopenTs,
                                                   epoch    |-> shardPrimaryEpoch'[s],
                                                   pos      |-> 0]]
    /\ UNCHANGED <<coordinatorTime, activeShards, shardAddedAt,
                   oplog_vars,
                   consumerResumeToken, consumerHWM, consumerDelivered>>

\* Action: the consumer issues a getMore against the cursor on shard s, consuming the next
\* event whose clusterTime is >= the cursor's openedAt timestamp.
\*
\* The cursor advances its position (pos) past events strictly below openedAt without emitting
\* them to the consumer — these are events the change-stream pipeline filters out before they
\* reach the user. Events at or above openedAt are delivered to the user.
\*
\* Post-batch, the consumer updates its high-water-mark to the max of its current HWM and the
\* delivered event's clusterTime. This models the postBatchResumeToken contract.
ConsumerGetMore(s) ==
    /\ s \in activeShards
    /\ shardCursor[s] # NoCursor
    /\ shardCursor[s].pos < Len(oplog[s])
    /\ LET entry      == oplog[s][shardCursor[s].pos + 1]
           cursorOpen == shardCursor[s].openedAt IN
        IF entry.ts >= cursorOpen
            THEN
                /\ consumerDelivered' = Append(consumerDelivered,
                                                [shard |-> s, ts |-> entry.ts])
                /\ consumerHWM' = Max(consumerHWM, entry.ts)
                /\ shardCursor' = [shardCursor EXCEPT ![s] = [@ EXCEPT !.pos = @ + 1]]
            ELSE
                \* The pipeline filters out this event without delivering it. The cursor still
                \* advances past it.
                /\ shardCursor' = [shardCursor EXCEPT ![s] = [@ EXCEPT !.pos = @ + 1]]
                /\ UNCHANGED <<consumerDelivered, consumerHWM>>
    /\ UNCHANGED <<coordinator_vars, oplog_vars, shard_vars, consumerResumeToken>>

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Next-state relation and fairness                                                               *)
(**************************************************************************************************)

Next ==
    \/ Tick
    \/ \E s \in ShardIds : WriteOnShard(s)
    \/ \E s \in ShardIds : AddShard(s)
    \/ \E s \in ShardIds : RemoveShard(s)
    \/ \E s \in ShardIds : Stepdown(s)
    \/ \E s \in ShardIds : ConsumerGetMore(s)

\* Strong fairness on consumer drains; weak fairness on time-bounded coordinator actions.
Fairness ==
    /\ \A s \in ShardIds : SF_vars(ConsumerGetMore(s))
    /\ WF_vars(\E s \in ShardIds : WriteOnShard(s))

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariant                                                                                 *)
(**************************************************************************************************)

TypeOK ==
    /\ coordinatorTime \in 0..MaxClusterTime
    /\ activeShards \subseteq ShardIds
    /\ \A s \in ShardIds : shardAddedAt[s] \in 0..MaxClusterTime
    /\ topologyChanges \in 0..MaxTopologyChanges
    /\ writes \in 0..MaxWrites
    /\ \A s \in ShardIds : shardPrimaryEpoch[s] \in Nat \ {0}
    /\ consumerResumeToken \in Nat
    /\ consumerHWM \in Nat
    /\ \A s \in ShardIds :
        \/ shardCursor[s] = NoCursor
        \/ /\ shardCursor[s].openedAt \in Nat
           /\ shardCursor[s].epoch \in Nat
           /\ shardCursor[s].pos \in 0..Len(oplog[s])

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Safety invariants                                                                              *)
(**************************************************************************************************)

\* SAFETY 1: the consumer never sees an event whose clusterTime is strictly less than its
\* resume-token clusterTime. This is violated by the SERVER-48386 / SERVER-124540 race when
\* a shard is added at a coordinator time below the future startAtOperationTime and the
\* new-shard cursor opens at shardAddedTime+1 without comparing against the resume token.
NoEventBeforeResumeToken ==
    \A i \in 1..Len(consumerDelivered) :
        consumerDelivered[i].ts >= consumerResumeToken

\* SAFETY 2: the consumer's high-water-mark resume token never falls below the resume token
\* the stream was opened at, and every delivered event has clusterTime <= the current HWM.
\* Strict monotonicity across steps (HWM never decreases) is structurally enforced by the
\* spec body (HWM only takes Max(_, entry.ts)) and can be additionally verified by enabling
\* the temporal property MonotonicHighWatermarkProperty below.
MonotonicHighWatermark ==
    /\ consumerHWM >= consumerResumeToken
    /\ \A i \in 1..Len(consumerDelivered) :
        consumerDelivered[i].ts <= consumerHWM

\* Helper: the cursor on every active shard sits at or above the resume token. This is the
\* operational invariant that, if maintained, implies NoEventBeforeResumeToken.
ShardCursorsAtOrAboveResumeToken ==
    \A s \in activeShards :
        \/ shardCursor[s] = NoCursor
        \/ shardCursor[s].openedAt >= consumerResumeToken

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Liveness                                                                                       *)
(**************************************************************************************************)

\* TEMPORAL: the HWM never decreases across any transition. Encoded as a TLA+ action property,
\* enabled via PROPERTY in the .cfg.
MonotonicHighWatermarkProperty ==
    [][consumerHWM' >= consumerHWM]_consumer_vars

\* Every oplog entry on an active shard at or above the resume token is eventually delivered to
\* the consumer (modulo stepdowns, which the fairness assumption above resolves).
EventuallyDelivered ==
    \A s \in ShardIds : \A i \in 1..MaxWrites :
        (    s \in activeShards
          /\ i <= Len(oplog[s])
          /\ oplog[s][i].ts >= consumerResumeToken)
        ~> \E j \in 1..Len(consumerDelivered) :
                consumerDelivered[j].shard = s /\ consumerDelivered[j].ts = oplog[s][i].ts

====================================================================================================
