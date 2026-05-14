\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE ChainedSecondaryOverRead ---------------------------
\* SERVER-120205: Chained secondary can read step-up no-op oplog entry beyond
\* new primary's oplog visibility timestamp.
\*
\* Bug summary:
\*   When a node S transitions Follower -> Leader, the replication coordinator
\*   first writes a step-up no-op oplog entry, and only afterwards updates the
\*   topology coordinator's "can accept writes" flag. The oplog visibility
\*   thread on S has not yet advanced the visibility timestamp past the no-op.
\*   A chained secondary T currently tailing S's oplog uses the kLastApplied
\*   read source (the kNoTimestamp read source is only re-acquired on the
\*   yield/restore that follows the topology flip). Therefore between the no-op
\*   write and the topology flip, T can fetch the no-op entry past S's
\*   visibility timestamp.
\*
\* Spec shape:
\*   * Three timestamps per server: lastApplied (top of oplog), oplogVisibility
\*     (what tailable cursors are allowed to return), and a boolean
\*     canAcceptWrites (topology coordinator's writeable bit).
\*   * Step-up is split into three sub-actions: StepUpWriteNoOp (lastApplied
\*     advances, state remains Follower-for-cursor-purposes, visibility lags),
\*     StepUpAdvanceVisibility (visibility catches up), StepUpFlipTopology
\*     (canAcceptWrites <- TRUE, cursors must re-acquire read source).
\*   * A chained secondary action AppendOplogFromSyncSource models a tailable
\*     oplog cursor fetching from its sync source. The read source is chosen by
\*     a predicate ReadGate(syncSource): kNoTimestamp when canAcceptWrites is
\*     TRUE (cursor restored after topology flip), kLastApplied otherwise. The
\*     bug toggle BugChainedSecondaryOverRead flips whether the cursor sees the
\*     no-op past visibility.
\*
\* Invariant of interest:
\*   NoOverReadBeyondVisibility - every chained secondary's lastApplied
\*   timestamp is <= the oplogVisibility of the sync source at the moment of
\*   the fetch. Holds when BugChainedSecondaryOverRead = FALSE. Violated when
\*   the bug toggle is TRUE, reproducing SERVER-120205.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/ChainedSecondaryOverRead

EXTENDS Integers, FiniteSets, Sequences, TLC

\* The set of server IDs.
CONSTANT Server

\* When TRUE, models the SERVER-120205 bug: a chained secondary's tailing
\* cursor on its sync source is allowed to read the step-up no-op entry past
\* the sync source's oplog visibility timestamp, because the cursor's read
\* source has not yet been re-acquired. When FALSE, the cursor is bounded by
\* the sync source's visibility (the fixed behavior).
CONSTANT BugChainedSecondaryOverRead

----
\* Per-server variables.

\* The server's term number.
VARIABLE currentTerm

\* The server's role for replication: "Follower" or "Leader". The topology
\* coordinator's writeable bit is canAcceptWrites; state["Leader"] alone does
\* not imply canAcceptWrites - that flip is a separate sub-action.
VARIABLE state

\* Per-server oplog. Each entry has a term and a kind ("client" or "noop").
VARIABLE log

\* Per-server top-of-oplog timestamp (term, index). Matches Len(log[s]) under
\* the convention that lastApplied advances synchronously with log writes on
\* a server; replication on a follower advances lastApplied via ApplyOplog.
VARIABLE lastApplied

\* Per-server oplog visibility timestamp. Cursors that read with the
\* kLastApplied read source are bounded by lastApplied; cursors that read with
\* the kNoTimestamp read source on a primary are bounded by oplogVisibility.
\* On a follower oplogVisibility tracks lastApplied. On a leader candidate
\* mid-step-up, oplogVisibility lags lastApplied until
\* StepUpAdvanceVisibility fires.
VARIABLE oplogVisibility

\* Topology coordinator's "this node can accept writes as primary" bit. Set
\* TRUE on StepUpFlipTopology; cursors yield/restore and re-acquire their read
\* source at this point.
VARIABLE canAcceptWrites

\* Each server's current sync source (or NoSyncSource).
VARIABLE syncSource

\* Set of (i, j, ts) facts recording that server i fetched from server j at
\* a moment when j's oplogVisibility was ts. Used by the safety invariant to
\* check no-over-read against the visibility at fetch time rather than the
\* eventual visibility.
VARIABLE fetchHistory

serverVars == <<currentTerm, state, canAcceptWrites>>
oplogVars == <<log, lastApplied, oplogVisibility>>
chainVars == <<syncSource, fetchHistory>>
vars == <<serverVars, oplogVars, chainVars>>

NoSyncSource == "NoSyncSource"

----
\* Helpers

IsMajority(servers) == Cardinality(servers) * 2 > Cardinality(Server)
Range(f) == {f[x] : x \in DOMAIN f}
GetTerm(xlog, index) == IF index = 0 THEN 0 ELSE xlog[index].term
LogTerm(i, index) == GetTerm(log[i], index)
LastTerm(xlog) == GetTerm(xlog, Len(xlog))
Leaders == {s \in Server : state[s] = "Leader"}

TS(t, i) == [term |-> t, index |-> i]
TopOf(s) == TS(LastTerm(log[s]), Len(log[s]))

\* Timestamp ordering.
TimestampLTE(x, y) ==
    \/ x.term < y.term
    \/ /\ x.term = y.term
       /\ x.index <= y.index

Max(s) == CHOOSE x \in s : \A y \in s : x >= y
GlobalCurrentTerm == Max(Range(currentTerm))

\* Server i is allowed to sync from server j (j strictly ahead, prefixes agree).
CanSyncFrom(i, j) ==
    /\ i # j
    /\ Len(log[i]) < Len(log[j])
    /\ LastTerm(log[i]) = LogTerm(j, Len(log[i]))

\* "Not behind" semantics for election eligibility.
NotBehind(me, j) ==
    \/ LastTerm(log[me]) > LastTerm(log[j])
    \/ /\ LastTerm(log[me]) = LastTerm(log[j])
       /\ Len(log[me]) >= Len(log[j])

\* The read-source the cursor on i tailing j actually has, modeled as the upper
\* bound on what i is allowed to read from j's log. When j's topology has been
\* flipped to writeable, cursors have been forced to yield and re-acquire as
\* kNoTimestamp, which on a primary is gated by oplogVisibility. When j has
\* not yet flipped, the cursor still holds kLastApplied: bounded by lastApplied
\* (the bug path) or by oplogVisibility (the fixed path), depending on the
\* bug toggle.
ReadUpperBound(j) ==
    IF canAcceptWrites[j]
        THEN oplogVisibility[j]
        ELSE IF BugChainedSecondaryOverRead
                THEN lastApplied[j]
                ELSE oplogVisibility[j]

----
\* Initial state. All servers start as followers in term 0 with empty logs.
Init ==
    /\ currentTerm     = [i \in Server |-> 0]
    /\ state           = [i \in Server |-> "Follower"]
    /\ canAcceptWrites = [i \in Server |-> FALSE]
    /\ log             = [i \in Server |-> << >>]
    /\ lastApplied     = [i \in Server |-> TS(0, 0)]
    /\ oplogVisibility = [i \in Server |-> TS(0, 0)]
    /\ syncSource      = [i \in Server |-> NoSyncSource]
    /\ fetchHistory    = {}

----
\* Actions.

\* A leader writes a client entry. Visibility on the leader advances when its
\* oplog visibility thread runs (AdvanceVisibility), not synchronously. The
\* MaxLogLen constraint (in the MC harness) bounds the total log size; here we
\* just allow as many client writes as the harness permits.
ClientWrite(i) ==
    /\ state[i] = "Leader"
    /\ canAcceptWrites[i] = TRUE
    /\ LET entry  == [term |-> currentTerm[i], kind |-> "client"]
           newLog == Append(log[i], entry)
       IN  /\ log'         = [log EXCEPT ![i] = newLog]
           /\ lastApplied' = [lastApplied EXCEPT ![i] = TS(LastTerm(newLog), Len(newLog))]
    /\ UNCHANGED <<currentTerm, state, canAcceptWrites, oplogVisibility, syncSource, fetchHistory>>

\* The oplog visibility thread advances visibility to lastApplied. On a primary
\* this is the moment at which kNoTimestamp readers can see the latest writes.
AdvanceVisibility(i) ==
    /\ oplogVisibility[i] # lastApplied[i]
    /\ TimestampLTE(oplogVisibility[i], lastApplied[i])
    /\ oplogVisibility' = [oplogVisibility EXCEPT ![i] = lastApplied[i]]
    /\ UNCHANGED <<currentTerm, state, canAcceptWrites, log, lastApplied, syncSource, fetchHistory>>

\* Election. Node i becomes a leader candidate by majority vote: state -> Leader
\* but canAcceptWrites is still FALSE. The step-up no-op write and topology
\* flip are separate sub-actions below.
BecomeLeaderCandidate(i, ayeVoters) ==
    /\ i \in ayeVoters
    /\ IsMajority(ayeVoters)
    /\ \A j \in ayeVoters : /\ NotBehind(i, j)
                            /\ currentTerm[j] <= currentTerm[i]
    /\ state[i] = "Follower"
    /\ state'           = [s \in Server |-> IF s \notin ayeVoters
                                            THEN state[s]
                                            ELSE IF s = i THEN "Leader" ELSE "Follower"]
    /\ currentTerm'     = [s \in Server |-> IF s \in ayeVoters
                                            THEN currentTerm[i] + 1
                                            ELSE currentTerm[s]]
    /\ canAcceptWrites' = [canAcceptWrites EXCEPT ![i] = FALSE]
    /\ UNCHANGED <<log, lastApplied, oplogVisibility, syncSource, fetchHistory>>

\* SUB-ACTION 1 of step-up: replication coordinator writes the step-up no-op
\* entry. lastApplied advances. canAcceptWrites is still FALSE.
\* oplogVisibility lags.
StepUpWriteNoOp(i) ==
    /\ state[i] = "Leader"
    /\ canAcceptWrites[i] = FALSE
    /\ \* The no-op is written at the new term.
       LET entry  == [term |-> currentTerm[i], kind |-> "noop"]
           newLog == Append(log[i], entry)
       IN  /\ log'         = [log EXCEPT ![i] = newLog]
           /\ lastApplied' = [lastApplied EXCEPT ![i] = TS(LastTerm(newLog), Len(newLog))]
    /\ \* In particular, oplogVisibility is NOT advanced here. This is the
       \* window during which SERVER-120205 reproduces.
       UNCHANGED <<currentTerm, state, canAcceptWrites, oplogVisibility, syncSource, fetchHistory>>

\* SUB-ACTION 2 of step-up: oplog visibility thread runs and advances
\* visibility past the no-op entry. Identical body to AdvanceVisibility, named
\* separately so the model checker can record the step-up boundary distinctly.
StepUpAdvanceVisibility(i) ==
    /\ state[i] = "Leader"
    /\ oplogVisibility[i] # lastApplied[i]
    /\ TimestampLTE(oplogVisibility[i], lastApplied[i])
    /\ oplogVisibility' = [oplogVisibility EXCEPT ![i] = lastApplied[i]]
    /\ UNCHANGED <<currentTerm, state, canAcceptWrites, log, lastApplied, syncSource, fetchHistory>>

\* SUB-ACTION 3 of step-up: topology coordinator flips canAcceptWrites to TRUE.
\* At this moment cursors on followers tailing i must yield/restore and
\* re-acquire kNoTimestamp; this is modeled by ReadUpperBound switching to
\* oplogVisibility[i] unconditionally.
StepUpFlipTopology(i) ==
    /\ state[i] = "Leader"
    /\ canAcceptWrites[i] = FALSE
    /\ canAcceptWrites' = [canAcceptWrites EXCEPT ![i] = TRUE]
    /\ UNCHANGED <<currentTerm, state, log, lastApplied, oplogVisibility, syncSource, fetchHistory>>

\* Pick or switch sync source. A follower may decide to sync from j whenever
\* j has more log than i.
ChooseSyncSource(i, j) ==
    /\ state[i] = "Follower"
    /\ CanSyncFrom(i, j)
    /\ syncSource' = [syncSource EXCEPT ![i] = j]
    /\ UNCHANGED <<serverVars, oplogVars, fetchHistory>>

\* A follower i pulls one or more entries from its sync source j. The read is
\* gated by ReadUpperBound(j): the index of the upper bound is the largest
\* index in j's log that the cursor is allowed to deliver right now.
AppendOplogFromSyncSource(i) ==
    /\ state[i] = "Follower"
    /\ syncSource[i] # NoSyncSource
    /\ LET j     == syncSource[i]
           bound == ReadUpperBound(j)
           upTo  == bound.index
       IN  /\ CanSyncFrom(i, j)
           /\ upTo > Len(log[i])
           /\ \E lastAppended \in (Len(log[i]) + 1)..upTo :
                  LET appended == SubSeq(log[j], Len(log[i]) + 1, lastAppended)
                      newLog   == log[i] \o appended
                  IN  /\ log'          = [log EXCEPT ![i] = newLog]
                      /\ lastApplied'  = [lastApplied EXCEPT ![i] =
                                             TS(LastTerm(newLog), Len(newLog))]
                      /\ oplogVisibility' = [oplogVisibility EXCEPT ![i] =
                                                TS(LastTerm(newLog), Len(newLog))]
                      /\ fetchHistory' = fetchHistory \union
                             {[reader |-> i,
                               source |-> j,
                               readUpTo |-> TS(LastTerm(newLog), Len(newLog)),
                               sourceVisibilityAtFetch |-> oplogVisibility[j]]}
    /\ UNCHANGED <<currentTerm, state, canAcceptWrites, syncSource>>

\* Term-bump heartbeat: a follower discovers a higher term, steps down (if
\* leader), and forgets writeability.
UpdateTermThroughHeartbeat(i, j) ==
    /\ currentTerm[j] > currentTerm[i]
    /\ currentTerm'     = [currentTerm EXCEPT ![i] = currentTerm[j]]
    /\ state'           = [state EXCEPT ![i] = "Follower"]
    /\ canAcceptWrites' = [canAcceptWrites EXCEPT ![i] = FALSE]
    /\ UNCHANGED <<oplogVars, syncSource, fetchHistory>>

----
\* Next-state relation.

ClientWriteAction              == \E i \in Server : ClientWrite(i)
AdvanceVisibilityAction        == \E i \in Server : AdvanceVisibility(i)
BecomeLeaderCandidateAction    == \E i \in Server : \E v \in SUBSET Server : BecomeLeaderCandidate(i, v)
StepUpWriteNoOpAction          == \E i \in Server : StepUpWriteNoOp(i)
StepUpAdvanceVisibilityAction  == \E i \in Server : StepUpAdvanceVisibility(i)
StepUpFlipTopologyAction       == \E i \in Server : StepUpFlipTopology(i)
ChooseSyncSourceAction         == \E i, j \in Server : ChooseSyncSource(i, j)
AppendOplogFromSyncSourceAction == \E i \in Server : AppendOplogFromSyncSource(i)
UpdateTermThroughHeartbeatAction == \E i, j \in Server : UpdateTermThroughHeartbeat(i, j)

Next ==
    \/ ClientWriteAction
    \/ AdvanceVisibilityAction
    \/ BecomeLeaderCandidateAction
    \/ StepUpWriteNoOpAction
    \/ StepUpAdvanceVisibilityAction
    \/ StepUpFlipTopologyAction
    \/ ChooseSyncSourceAction
    \/ AppendOplogFromSyncSourceAction
    \/ UpdateTermThroughHeartbeatAction

Spec == Init /\ [][Next]_vars

----
\* Properties.

\* SERVER-120205 safety invariant: no chained secondary ever ends up with a
\* lastApplied past its sync source's oplog visibility at the moment of the
\* fetch. (We check against the visibility AT fetch time because the source
\* may have advanced visibility afterwards; the bug is about the over-read,
\* not the eventual state.)
NoOverReadBeyondVisibility ==
    \A f \in fetchHistory :
        TimestampLTE(f.readUpTo, f.sourceVisibilityAtFetch)

\* The standard MongoDB Raft invariant kept as a regression guard.
NoTwoPrimariesInSameTerm ==
    \A i, j \in Server :
        (i # j /\ state[i] = "Leader" /\ state[j] = "Leader")
            => currentTerm[i] # currentTerm[j]

\* writeability implies the topology flip has happened; the converse need not.
WriteabilityImpliesLeader ==
    \A i \in Server : canAcceptWrites[i] => state[i] = "Leader"

\* Visibility never exceeds lastApplied (the structural rule violated in the
\* bug window only for cursor reads, not for the local node state).
VisibilityNeverExceedsLastApplied ==
    \A i \in Server : TimestampLTE(oplogVisibility[i], lastApplied[i])

=============================================================================
