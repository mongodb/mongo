-------------------------- MODULE ExactInitialSync --------------------------
\* Exact initial sync.
\*
\* A model of the per-partition copy / route / flush procedure for exact
\* initial sync: snapshot a source while it is being mutated, then switch to
\* a change stream and apply every event exactly once. The design relies on
\* the following assumptions marked [A1]..[A4].
\*
\*  [A1] afterClusterTime: every document returned by the find reflects
\*       the source at some time >= tpause.
\*  [A2] operationTime: every document returned reflects the source at
\*       some time <= tcopy.
\*  [A3] The change stream is an exact, ordered, exactly-once history of
\*       committed writes.
\*  [A4] Change events carry a full post-image as of the event itself.
\*
\* Design mapping
\*   source                 -> src
\*   oplog / clusterTime    -> oplog, index = clusterTime
\*   change stream position -> streamPos
\*   partition states       -> pstate (in-flight is split into "copying"
\*                             = copy running, "inflight" = awaiting flush)
\*   fuzzy find()           -> ReadOne, one key per step, value chosen
\*                             from any time in [tpause, now]
\*   buffer                 -> buf
\*   downstream consumer    -> sink, plus preOk which records whether
\*                             every emitted event matched sink's pre-image
\*   checkpoint / restart   -> ckpt, Checkpoint, CrashAndRestore
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
  Keys,     \* the key space, e.g. {1, 2, 3}
  Vals,     \* document "contents", e.g. {1, 2}
  Parts,    \* sequence of disjoint, nonempty key sets covering Keys
  MaxOps,   \* bound on oplog length; keeps the state space finite
  NULL      \* model value meaning "document absent"

ASSUME NULL \notin Vals
ASSUME UNION {Parts[i] : i \in 1..Len(Parts)} = Keys \* assume parts cover the whole key space
ASSUME \A i \in 1..Len(Parts) : Parts[i] # {} \* each part is non-empty -- # {} means not equal to the empty set
ASSUME \A i, j \in 1..Len(Parts) : i # j => Parts[i] \cap Parts[j] = {} \* the intersection of any two part sets is the empty set, i.e. no key belongs to two different parts

NumParts  == Len(Parts)
PartIds   == 1..NumParts
Doc       == Vals \cup {NULL}
\* "copying" state isn't explicitly mentioned in the design, but it's
\* when we pull all data for a partition into the buffer.
\* in the future we could execute the find()s and read from the change stream
\* at the same time, and then read more change stream data after the find()s
\* execute if we haven't read up to operationTime yet. worth considering later on
\* as a potential performance improvement.
PStates   == {"pending", "copying", "inflight", "completed"}
PartOf(k) == CHOOSE p \in PartIds : k \in Parts[p]

VARIABLES
  src,        \* [Keys -> Doc]        current source collection
  oplog,      \* Seq of [key, pre, post]; oplog[t] committed at clusterTime t
  hist,       \* hist[t+1] = src after oplog[t]; hist[1] = initial src
  streamPos,  \* index of the last oplog entry consumed by the change stream
  pstate,     \* [PartIds -> PStates]
  tpause,     \* stream position when the current copy started
  unread,     \* keys of the copying partition the find has not returned yet
  tcopy,      \* upper bound (operationTime) of the in-flight partition's find
  buf,        \* [Keys -> Doc]        the in-flight partition's buffer
  sink,       \* [Keys -> Doc]        what downstream currently holds
  preOk,      \* FALSE once any emitted event conflicts with sink
  ckpt        \* [pos, completed, sink]  the last checkpoint

vars == <<src, oplog, hist, streamPos, pstate, tpause, unread, tcopy,
          buf, sink, preOk, ckpt>> \* bookkeeping, defines vars as the tuple of all state variables

Empty    == [k \in Keys |-> NULL]
SrcAt(t) == hist[t + 1]        \* source state as of clusterTime t

Copying   == \E p \in PartIds : pstate[p] = "copying"
\* flush when there exists an inflight partition and stream position is equal to tcopy
MustFlush == \E p \in PartIds : pstate[p] = "inflight" /\ streamPos = tcopy

-----------------------------------------------------------------------------
\* Initial state: any collection contents, nothing synced yet.

Init ==
  /\ src       \in [Keys -> Doc]
  /\ oplog     = << >>
  \* hist is a sequence of full snapshots: hist[i] is the entire collection
  \* as it looked after i-1 writes
  /\ hist      = << src >>
  /\ streamPos = 0
  /\ pstate    = [p \in PartIds |-> "pending"]
  /\ tpause    = 0
  /\ unread    = {}
  /\ tcopy     = 0
  /\ buf       = Empty
  /\ sink      = Empty
  /\ preOk     = TRUE
  /\ ckpt      = [pos |-> 0, completed |-> {}, sink |-> Empty]

-----------------------------------------------------------------------------
\* A writer commits an insert, update, or delete
Write ==
  \* there are still writes available
  /\ Len(oplog) < MaxOps
  \* there exist a key and value combination where
  /\ \E k \in Keys, post \in Doc :
       \* the value (post) does not equal src[k] (the value of k in the current collection)
       \* i.e. the write actually changes something
       /\ post # src[k]
       \* and the new oplog equals the old oplog plus the new value
       /\ oplog' = Append(oplog, [key |-> k, pre |-> src[k], post |-> post])
       \* produces a new src identical to the old one except src[k] now maps to post
       /\ src'   = [src EXCEPT ![k] = post]
       \* sequence of snapshots now has this new src snapshot
       /\ hist'  = Append(hist, src')
  \* other variables remain unchanged
  /\ UNCHANGED <<streamPos, pstate, tpause, unread, tcopy, buf, sink, preOk, ckpt>>

-----------------------------------------------------------------------------
\* Step 1-2: pause the stream and start the find for one pending partition.
\* Sequential version: only one partition is copying/in-flight at a time.
\* We can extend this later to have multiple in-flight partitions.

StartCopy ==
  \* all partitions are currently either in pending or completed state, i.e. no copying or in-flight partitions
  /\ \A q \in PartIds : pstate[q] \in {"pending", "completed"}
  \* we find a partition in pending state
  /\ \E p \in PartIds :
       /\ pstate[p] = "pending"
       \* update that partition's pstate to copying
       /\ pstate' = [pstate EXCEPT ![p] = "copying"]
       \* set the pause point
       /\ tpause' = streamPos
       \* mark all docs that exist in that partition as unread
       /\ unread' = Parts[p]
       /\ buf'    = Empty
  /\ UNCHANGED <<src, oplog, hist, streamPos, tcopy, sink, preOk, ckpt>>

\* ReadOne runs the copying phase, which just populates the buffer for the current copying partition.
\* Each key is returned in some arbitrary order, reflecting the source at an arbitrary time t in [tpause, now]
ReadOne ==
  \* find a partition and an unread key
  /\ \E p \in PartIds, k \in unread :
       \* the unread key is for the partition in copying phase
       /\ pstate[p] = "copying"
       \* update the buffer with the state of the unread key, which can be the state of
       \* that key's value at any point in time between tpause and the end of the oplog.
       \* this is what models the unpredictability of the results returned by the find()
       /\ \E t \in tpause..Len(oplog) :
            buf' = [buf EXCEPT ![k] = SrcAt(t)[k]]
       \* remove the unread key from the unread set
       /\ unread' = unread \ {k}
       \* and if it was the last one in the set, move that partition from the copying phase
       \* to the inflight phase.
       /\ IF unread = {k}
            THEN /\ tcopy'  = Len(oplog)
                 /\ pstate' = [pstate EXCEPT ![p] = "inflight"]
            ELSE UNCHANGED <<tcopy, pstate>>
  /\ UNCHANGED <<src, oplog, hist, streamPos, tpause, sink, preOk, ckpt>>

-----------------------------------------------------------------------------
\* Step 3: consume one change event and route it by partition state.
\* ~Copying encodes the design rule that no stream events are routed while a
\* copy is in progress. ~MustFlush forces the flush to happen before any
\* event with clusterTime > tcopy is routed. In the "inflight" branch the
\* buffer takes the event's post-image, which is [A4].

\* StreamStep runs the change stream reading phase, which consumes and routes events based on their owning partition's state
StreamStep ==
  \* no partitions are in copying phase
  /\ ~Copying
  \* no partition currently needs to flush
  /\ ~MustFlush
  /\ streamPos < Len(oplog)
  \* shorthand to specify e as the next unconsumed event, p as the partition that the event's key belongs to
  /\ LET e == oplog[streamPos + 1]
         p == PartOf(e.key)
     \* three possible partition states that handle the writes
     IN \/ /\ pstate[p] = "completed"
           \* completed partition gets the value propagated downstream, i.e. just update the sink
           \* enforce that the sink's current state reflects the event's preimage before updating
           /\ preOk' = (preOk /\ sink[e.key] = e.pre)
           /\ sink'  = [sink EXCEPT ![e.key] = e.post]
           /\ UNCHANGED buf
        \/ /\ pstate[p] = "pending"
           \* pending partition drops the writes, i.e. does nothing. the upcoming copy will capture it
           \* when we eventually process that partition
           /\ UNCHANGED <<buf, sink, preOk>>
        \/ /\ pstate[p] = "inflight"
           \* in-flight partition updates the buffer value
           /\ buf' = [buf EXCEPT ![e.key] = e.post]
           /\ UNCHANGED <<sink, preOk>>
  /\ streamPos' = streamPos + 1
  /\ UNCHANGED <<src, oplog, hist, pstate, tpause, unread, tcopy, ckpt>>

\* Step 4: every event with clusterTime <= tcopy has been merged, so the
\* buffer is the partition as of tcopy. Emit it as inserts and complete.

\* flushes the data for the just finished in-flight partition
Flush ==
  /\ \E p \in PartIds :
       \* find the in-flight partition
       /\ pstate[p] = "inflight"
       /\ streamPos = tcopy
       \* mark it as completed
       /\ pstate' = [pstate EXCEPT ![p] = "completed"]
       \* enforce that if the buffer holds a document for k, the sink must not have a value for k
       /\ preOk'  = (preOk /\ \A k \in Parts[p] : buf[k] # NULL => sink[k] = NULL)
       \* update the sink with all the values in the buffer
       /\ sink'   = [k \in Keys |-> IF k \in Parts[p] /\ buf[k] # NULL
                                      THEN buf[k] ELSE sink[k]]
  \* the buffer has been consumed by the sink, clear it
  /\ buf' = Empty
  /\ UNCHANGED <<src, oplog, hist, streamPos, tpause, unread, tcopy, ckpt>>

-----------------------------------------------------------------------------

\* checkpoint to persist the state which will be restored via CrashAndRestore
\* the three values are persisted atomically
Checkpoint ==
  /\ ckpt' = [pos       |-> streamPos,
              completed |-> {p \in PartIds : pstate[p] = "completed"},
              sink      |-> sink]
  /\ UNCHANGED <<src, oplog, hist, streamPos, pstate, tpause, unread, tcopy,
                 buf, sink, preOk>>

\* crash and restore from checkpointed state
CrashAndRestore ==
  /\ streamPos' = ckpt.pos
  /\ pstate'    = [p \in PartIds |-> IF p \in ckpt.completed
                                       THEN "completed" ELSE "pending"]
  /\ sink'      = ckpt.sink
  /\ buf'       = Empty
  /\ unread'    = {}
  /\ UNCHANGED <<src, oplog, hist, tpause, tcopy, preOk, ckpt>>

-----------------------------------------------------------------------------
Next == Write \/ StartCopy \/ ReadOne \/ StreamStep \/ Flush \/ Checkpoint \/ CrashAndRestore

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
\* Invariants

\* valid types and ranges of every variable
TypeOK ==
  /\ src       \in [Keys -> Doc]
  /\ Len(oplog) <= MaxOps
  /\ Len(hist) = Len(oplog) + 1
  /\ streamPos \in 0..Len(oplog)
  /\ pstate    \in [PartIds -> PStates]
  /\ tpause    \in 0..MaxOps
  /\ unread    \subseteq Keys
  /\ tcopy     \in 0..MaxOps
  /\ buf       \in [Keys -> Doc]
  /\ sink      \in [Keys -> Doc]
  /\ preOk     \in BOOLEAN

\* I1. Nothing emitted ever conflicted with what downstream already held:
\*     no duplicate inserts, every change's pre-image matched. This is
\*     the "no double counting" property for the consumer
NoBadEmission == preOk

\* I2. Downstream mirrors the source exactly for completed keys, as of the
\*     stream position, and knows nothing about the others.
SinkTracksSource ==
  \A k \in Keys :
    IF pstate[PartOf(k)] = "completed"
      THEN sink[k] = SrcAt(streamPos)[k]
      ELSE sink[k] = NULL

\* I3. The flush lemma: when the stream reaches tcopy, the buffer is the
\*     partition's state as of tcopy.
FlushLemma ==
  \A p \in PartIds :
    (pstate[p] = "inflight" /\ streamPos = tcopy) =>
      \A k \in Parts[p] : buf[k] = SrcAt(tcopy)[k]

\* Sanity check, not a real invariant. Check NotDone as an invariant and
\* TLC will "fail" with a trace that reaches a fully synced state for
\* sanity checking the model behavior
Done ==
    /\ \A p \in PartIds : pstate[p] = "completed"
    /\ streamPos = Len(oplog)

NotDone == ~Done

=============================================================================
