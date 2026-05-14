\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------- MODULE BucketCatalogConcurrentDrop ----------------------
\* Formal specification of the time-series bucket catalog state under concurrent
\* inserts and collection drops (SERVER-107351).
\*
\* Background. The bucket catalog keeps two pieces of per-collection-UUID state
\* the insert path mutates without holding the collection-level lock:
\*
\*   1. executionStats[uuid]        - lookup/insert via getOrInitializeExecutionStats
\*   2. open buckets keyed by uuid  - written by stageInsertBatch
\*
\* drop(uuid) is implemented in two phases (bucket_catalog.cpp::drop):
\*
\*     phase R (release stats):  catalog.executionStats.erase(uuid)
\*     phase C (clear buckets):  bucketStateRegistry.clearedSets[++era] := {uuid}
\*
\* Each phase is locked individually, but no lock spans both. A concurrent
\* insert that runs getOrInitializeExecutionStats(uuid) BETWEEN drop's R and C
\* phases re-emplaces an executionStats entry for the dropped uuid. The same
\* race lets an insert open a bucket against the (about-to-be-cleared) uuid,
\* leaving a dangling open-bucket reference after drop completes.
\*
\* We model:
\*   - Insert as a four-step state machine (lookupStats, openBucket,
\*     writeMeasurement, finishBatch).
\*   - Drop as the two-phase (releaseStats, clearBuckets) op above.
\*   - Two correctness invariants:
\*       NoStatsForDroppedCollection - executionStats has no entry whose
\*                                     uuid is in the drop history.
\*       NoOpenBucketForDroppedCollection - openBuckets has no entry whose
\*                                          uuid is in the drop history.
\*
\* The "bug" model configuration (MC*.cfg with AllowInterleavedDropPhases=TRUE)
\* allows insert steps to interleave between drop's R and C phases. TLC must
\* find a counterexample violating the invariants above.
\*
\* The "fixed" model configuration (AllowInterleavedDropPhases=FALSE) models
\* an atomic drop (the candidate fix: hold catalog.mutex across both R and C,
\* or re-clean executionStats entries written during the race window). The
\* invariants must hold.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh TimeSeries/BucketCatalogConcurrentDrop

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Collections,                  \* Set of collection-uuid identifiers (e.g. {c1, c2}).
    Inserters,                    \* Set of inserter-thread ids.
    Droppers,                     \* Set of dropper-thread ids.
    MaxDrops,                     \* Cap on total drop operations (state-space bound).
    AllowInterleavedDropPhases    \* TRUE  -> bug model: insert can interleave between R and C.
                                  \* FALSE -> fixed model: drop's R..C is atomic w.r.t. inserts.

ASSUME Cardinality(Collections) >= 1
ASSUME Cardinality(Inserters) >= 1
ASSUME Cardinality(Droppers) >= 1
ASSUME MaxDrops \in Nat
ASSUME AllowInterleavedDropPhases \in BOOLEAN

-----------------------------------------------------------------------------
\* Thread step enumeration. An inserter's "pc" tracks where in the insert
\* state machine it currently is. A dropper's "pc" tracks where in the drop
\* state machine it currently is.

InserterIdle      == "InserterIdle"
InserterPicked    == "InserterPicked"      \* chose a uuid, not yet acquired stats
InserterHasStats  == "InserterHasStats"    \* stats entry observed/created
InserterHasBucket == "InserterHasBucket"   \* open bucket recorded

DropperIdle       == "DropperIdle"
DropperReleased   == "DropperReleased"     \* completed phase R, not yet phase C

InserterStates == {InserterIdle, InserterPicked, InserterHasStats, InserterHasBucket}
DropperStates  == {DropperIdle, DropperReleased}

-----------------------------------------------------------------------------
VARIABLES
    executionStats,   \* Set of UUIDs currently in catalog.executionStats.
    openBuckets,      \* Set of UUIDs that currently have at least one open bucket.
    droppedHistory,   \* Sequence of UUIDs that have completed a drop (R+C).
    dropCount,        \* Total drop operations started (bound by MaxDrops).
    inserterPc,       \* [t \in Inserters -> InserterStates]
    inserterUuid,     \* [t \in Inserters -> Collections \cup {NoUuid}] uuid this thread is operating on
    dropperPc,        \* [t \in Droppers -> DropperStates]
    dropperUuid       \* [t \in Droppers -> Collections \cup {NoUuid}]

NoUuid == "<none>"
UuidOrNone == Collections \cup {NoUuid}

vars == <<executionStats, openBuckets, droppedHistory, dropCount,
          inserterPc, inserterUuid, dropperPc, dropperUuid>>

-----------------------------------------------------------------------------
\* When AllowInterleavedDropPhases = FALSE, inserts may not run while any
\* dropper is mid-flight (between phase R and phase C). This models the fix:
\* hold catalog.mutex across both phases, or re-sweep executionStats after
\* clearBuckets to purge any races.

AnyDropperMidFlight ==
    \E d \in Droppers : dropperPc[d] = DropperReleased

InsertersUnblocked ==
    AllowInterleavedDropPhases \/ ~AnyDropperMidFlight

-----------------------------------------------------------------------------
Init ==
    /\ executionStats = {}
    /\ openBuckets = {}
    /\ droppedHistory = <<>>
    /\ dropCount = 0
    /\ inserterPc = [t \in Inserters |-> InserterIdle]
    /\ inserterUuid = [t \in Inserters |-> NoUuid]
    /\ dropperPc = [t \in Droppers |-> DropperIdle]
    /\ dropperUuid = [t \in Droppers |-> NoUuid]

-----------------------------------------------------------------------------
\* Insert state machine.
\*
\* Step 1: pickCollection - choose a target uuid for this insert. No catalog
\* mutation yet.
PickCollection(t, uuid) ==
    /\ inserterPc[t] = InserterIdle
    /\ uuid \in Collections
    /\ inserterPc' = [inserterPc EXCEPT ![t] = InserterPicked]
    /\ inserterUuid' = [inserterUuid EXCEPT ![t] = uuid]
    /\ UNCHANGED <<executionStats, openBuckets, droppedHistory, dropCount,
                   dropperPc, dropperUuid>>

\* Step 2: lookupOrInitStats - acquires catalog.mutex briefly, observes
\* executionStats[uuid], emplaces if missing. This is the source of the
\* leak: if a drop's release-phase already ran and clear-phase hasn't yet,
\* this re-inserts a stats entry for the doomed uuid.
LookupOrInitStats(t) ==
    /\ inserterPc[t] = InserterPicked
    /\ InsertersUnblocked
    /\ LET u == inserterUuid[t]
       IN /\ executionStats' = executionStats \cup {u}
          /\ inserterPc' = [inserterPc EXCEPT ![t] = InserterHasStats]
    /\ UNCHANGED <<openBuckets, droppedHistory, dropCount,
                   inserterUuid, dropperPc, dropperUuid>>

\* Step 3: openBucket - stageInsertBatch eventually writes to the open-bucket
\* structures under the stripe lock. Same race exists: a drop in flight may
\* be about to clear buckets for this uuid, but hasn't yet.
OpenBucket(t) ==
    /\ inserterPc[t] = InserterHasStats
    /\ InsertersUnblocked
    /\ LET u == inserterUuid[t]
       IN /\ openBuckets' = openBuckets \cup {u}
          /\ inserterPc' = [inserterPc EXCEPT ![t] = InserterHasBucket]
    /\ UNCHANGED <<executionStats, droppedHistory, dropCount,
                   inserterUuid, dropperPc, dropperUuid>>

\* Step 4: finishBatch - the insert thread releases its references and returns
\* to idle. Bucket may remain open in the catalog for future inserts to
\* re-use; the in-flight thread state is what resets.
FinishBatch(t) ==
    /\ inserterPc[t] = InserterHasBucket
    /\ inserterPc' = [inserterPc EXCEPT ![t] = InserterIdle]
    /\ inserterUuid' = [inserterUuid EXCEPT ![t] = NoUuid]
    /\ UNCHANGED <<executionStats, openBuckets, droppedHistory, dropCount,
                   dropperPc, dropperUuid>>

-----------------------------------------------------------------------------
\* Drop state machine. The drop happens in two phases, modelling
\* releaseExecutionStatsFromBucketCatalog (phase R) and clearSetOfBuckets
\* (phase C). Each phase individually takes a relevant mutex, but the
\* current implementation does not hold a single lock that spans both,
\* which is the SERVER-107351 race.

\* Phase R: releaseExecutionStatsFromBucketCatalog(uuid)
\*   Lock catalog.mutex
\*   executionStats.erase(uuid)
ReleaseStats(d, uuid) ==
    /\ dropperPc[d] = DropperIdle
    /\ dropCount < MaxDrops
    /\ uuid \in Collections
    /\ executionStats' = executionStats \ {uuid}
    /\ dropperPc' = [dropperPc EXCEPT ![d] = DropperReleased]
    /\ dropperUuid' = [dropperUuid EXCEPT ![d] = uuid]
    /\ dropCount' = dropCount + 1
    /\ UNCHANGED <<openBuckets, droppedHistory,
                   inserterPc, inserterUuid>>

\* Phase C: clearSetOfBuckets(uuid)
\*   Lock bucketStateRegistry.mutex
\*   clear all bucket state referencing uuid
\* Then complete the drop and record it in history.
ClearBuckets(d) ==
    /\ dropperPc[d] = DropperReleased
    /\ LET u == dropperUuid[d]
       IN /\ openBuckets' = openBuckets \ {u}
          /\ droppedHistory' = Append(droppedHistory, u)
          /\ dropperPc' = [dropperPc EXCEPT ![d] = DropperIdle]
          /\ dropperUuid' = [dropperUuid EXCEPT ![d] = NoUuid]
    /\ UNCHANGED <<executionStats, dropCount,
                   inserterPc, inserterUuid>>

-----------------------------------------------------------------------------
Next ==
    \/ \E t \in Inserters, u \in Collections : PickCollection(t, u)
    \/ \E t \in Inserters : LookupOrInitStats(t)
    \/ \E t \in Inserters : OpenBucket(t)
    \/ \E t \in Inserters : FinishBatch(t)
    \/ \E d \in Droppers, u \in Collections : ReleaseStats(d, u)
    \/ \E d \in Droppers : ClearBuckets(d)

Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(Next)
    /\ \A t \in Inserters : WF_vars(LookupOrInitStats(t))
                            /\ WF_vars(OpenBucket(t))
                            /\ WF_vars(FinishBatch(t))
    /\ \A d \in Droppers : WF_vars(ClearBuckets(d))

-----------------------------------------------------------------------------
\* Invariants

TypeOK ==
    /\ executionStats \subseteq Collections
    /\ openBuckets \subseteq Collections
    /\ dropCount \in Nat
    /\ inserterPc \in [Inserters -> InserterStates]
    /\ inserterUuid \in [Inserters -> UuidOrNone]
    /\ dropperPc \in [Droppers -> DropperStates]
    /\ dropperUuid \in [Droppers -> UuidOrNone]
    /\ \A i \in 1..Len(droppedHistory) : droppedHistory[i] \in Collections

\* Set of uuids that have completed at least one drop and currently have no
\* in-flight insert holding a reference. (A re-create after drop is not
\* modelled; this spec treats each drop as terminal for its uuid.)
DroppedUuids == { droppedHistory[i] : i \in 1..Len(droppedHistory) }

\* No executionStats entry should reference a uuid whose drop has completed,
\* unless an in-flight insert is currently between LookupOrInitStats and
\* FinishBatch on that uuid AND it raced in (which itself is the bug).
\* The strict reading: once drop finishes (phase C done, history appended),
\* the catalog must hold no stats for that uuid.
NoStatsForDroppedCollection ==
    \A u \in DroppedUuids : u \notin executionStats

\* Likewise for open buckets: no open-bucket entry should reference a dropped
\* uuid after drop has completed.
NoOpenBucketForDroppedCollection ==
    \A u \in DroppedUuids : u \notin openBuckets

\* Composite invariant matching the ticket's wording: "post-drop, no open
\* bucket entries reference the dropped collection".
CatalogConsistentPostDrop ==
    /\ NoStatsForDroppedCollection
    /\ NoOpenBucketForDroppedCollection

\* A weaker liveness-flavoured invariant: a drop in flight must eventually
\* complete. Used by liveness checking.
DropEventuallyCompletes ==
    \A d \in Droppers :
        (dropperPc[d] = DropperReleased) ~> (dropperPc[d] = DropperIdle)

=============================================================================
