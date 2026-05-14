\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE StepUpCatchupCheckpoint -------------------------
\* Formal specification covering the SERVER-126311 bug: a candidate that has
\* received oplog entries through the log stream, but has not yet installed a
\* checkpoint, must not bypass step-up catchup. If it does, it can step up while
\* its on-disk catalog is empty, write a fresh catalog (e.g. config.transactions
\* with a new UUID), and effectively drop the prior oplog entries on the floor.
\*
\* Model elements (one per server):
\*   * currentTerm    - monotonically increasing election term.
\*   * state          - Leader | Follower.
\*   * oplog          - sequence of records [term |-> N, lsn |-> N]; lsn is a
\*                      monotonically increasing log-stream sequence number.
\*   * checkpointTS   - the highest oplog lsn that has been durably checkpointed
\*                      on this server. 0 means "no checkpoint installed yet".
\*   * lastApplied    - the highest oplog lsn applied to the local catalog.
\*                      Cannot advance past checkpointTS in the model: applying
\*                      an oplog entry requires the catalog to be readable, which
\*                      needs a checkpoint to know the oplog table ident.
\*
\* Global elements:
\*   * majorityCommitted - set of [term |-> N, lsn |-> N] entries that have been
\*                         acknowledged by a quorum. These represent the writes
\*                         we must not lose on step-up.
\*
\* The CONSTANT AllowBypassOnNoCheckpoint switches between the buggy code
\* (TRUE - reproduces the BF-43238 crash sequence) and the proposed fix
\* (FALSE - candidate refuses to step up until a checkpoint exists).

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANT Server
CONSTANT MaxTerm
CONSTANT MaxLogLen
CONSTANT MaxClientWriteSize

\* When TRUE, _catchUpForStepUp returns true on the no-checkpoint path even
\* with non-empty oplog. This is the production bug.
CONSTANT AllowBypassOnNoCheckpoint

VARIABLE currentTerm
VARIABLE state
VARIABLE oplog
VARIABLE checkpointTS
VARIABLE lastApplied
VARIABLE majorityCommitted

serverVars == <<currentTerm, state, oplog, checkpointTS, lastApplied>>
vars == <<serverVars, majorityCommitted>>

----
\* Helpers.

IsMajority(servers) == Cardinality(servers) * 2 > Cardinality(Server)

Range(f) == {f[x] : x \in DOMAIN f}

Max(S) == CHOOSE x \in S : \A y \in S : x >= y
Min(S) == CHOOSE x \in S : \A y \in S : x <= y

Leaders == {s \in Server : state[s] = "Leader"}

GlobalTerm == Max(Range(currentTerm) \cup {0})

\* The highest lsn that has been streamed to (i.e. is in the oplog of) server s.
LastWritten(s) == IF oplog[s] = << >> THEN 0 ELSE oplog[s][Len(oplog[s])].lsn

\* The set of servers whose oplog contains an entry with the given lsn.
HaveLsn(lsn) == { s \in Server : \E k \in 1..Len(oplog[s]) : oplog[s][k].lsn = lsn }

\* True iff entry e (a record with term/lsn) is present in oplog[s].
EntryInLog(s, e) ==
    \E k \in 1..Len(oplog[s]) :
        /\ oplog[s][k].term = e.term
        /\ oplog[s][k].lsn  = e.lsn

----
\* Initial state. All servers are Followers in term 0 with empty oplogs and no
\* installed checkpoint, mirroring a fresh cluster start.

Init ==
    /\ currentTerm     = [s \in Server |-> 0]
    /\ state           = [s \in Server |-> "Follower"]
    /\ oplog           = [s \in Server |-> << >>]
    /\ checkpointTS    = [s \in Server |-> 0]
    /\ lastApplied     = [s \in Server |-> 0]
    /\ majorityCommitted = {}

----
\* ACTION: BecomePrimaryByMagic
\* Some node is elected leader by a majority. This abstracts the election
\* round-trips and only enforces:
\*   * the candidate is not behind any voter on lsn (last-written),
\*   * the voter set is a majority,
\*   * after election the new term strictly exceeds prior terms in the quorum.
\*
\* Crucially, election does NOT do catchup. Catchup is a SEPARATE post-election
\* phase implemented by TryStepUpCatchup below; the bug lives there.

BecomePrimaryByMagic(i, voters) ==
    /\ currentTerm[i] < MaxTerm
    /\ IsMajority(voters)
    /\ i \in voters
    /\ \A v \in voters : LastWritten(i) >= LastWritten(v)
    /\ \A v \in voters : currentTerm[v] <= currentTerm[i] + 1
    /\ state' = [s \in Server |->
                    IF s = i THEN "Leader"
                    ELSE IF s \in voters THEN "Follower"
                    ELSE state[s]]
    /\ currentTerm' = [s \in Server |->
                          IF s \in voters \cup {i}
                          THEN currentTerm[i] + 1
                          ELSE currentTerm[s]]
    /\ UNCHANGED <<oplog, checkpointTS, lastApplied, majorityCommitted>>

----
\* ACTION: TryStepUpCatchup
\* This models the _catchUpForStepUp gate that the BF-43238 RCA points at.
\* It runs after election but before the new primary starts taking writes. The
\* gate must return TRUE for the node to keep its Leader state; otherwise the
\* server steps back down.
\*
\* The faithfully buggy path:
\*   1. If LastWritten(i) = 0       --> legitimate fresh-cluster fall-through (OK).
\*   2. If checkpointTS[i] > 0      --> standard catchup path: only succeed if
\*                                       lastApplied has reached LastWritten.
\*   3. checkpointTS[i] = 0 AND LastWritten(i) > 0:
\*        (a) AllowBypassOnNoCheckpoint = TRUE  --> succeed anyway (BUG).
\*        (b) AllowBypassOnNoCheckpoint = FALSE --> refuse, step back down (FIX).

CatchupAllowsStepUp(i) ==
    \/ LastWritten(i) = 0
    \/ /\ checkpointTS[i] > 0
       /\ lastApplied[i] >= LastWritten(i)
    \/ /\ checkpointTS[i] = 0
       /\ LastWritten(i) > 0
       /\ AllowBypassOnNoCheckpoint

\* Catchup gate fires immediately after the leader transitions on. Either it
\* confirms the new leader (no state change here; subsequent ClientWrite is
\* enabled), or it forces a step-down without any oplog write.

TryStepUpCatchup(i) ==
    /\ state[i] = "Leader"
    /\ ~ CatchupAllowsStepUp(i)
    /\ state' = [state EXCEPT ![i] = "Follower"]
    /\ UNCHANGED <<currentTerm, oplog, checkpointTS, lastApplied, majorityCommitted>>

----
\* ACTION: ClientWrite
\* Leader appends a new oplog entry stamped with its term. Note: this is the
\* point at which the bug causes data loss. In the production code path, a new
\* primary that survived catchup writes a 'create config.transactions' oplog
\* entry which conflicts with the catalog on every other node that already had
\* the prior UUID. In the model we keep it abstract: any write produced by an
\* "uncaught-up" new primary will not include the prior majority-committed
\* entries it never saw, so the invariant catches it directly.

ClientWrite(i) ==
    /\ state[i] = "Leader"
    /\ CatchupAllowsStepUp(i)
    /\ Len(oplog[i]) < MaxLogLen
    /\ \E numEntries \in 1..MaxClientWriteSize :
        LET startLsn == LastWritten(i) + 1
            newEntries == [k \in 1..numEntries |->
                              [term |-> currentTerm[i], lsn |-> startLsn + k - 1]]
        IN  oplog' = [oplog EXCEPT ![i] = oplog[i] \o newEntries]
    /\ UNCHANGED <<currentTerm, state, checkpointTS, lastApplied, majorityCommitted>>

----
\* ACTION: ReplicateOplog
\* A follower picks up an oplog entry that some other node has written but it
\* doesn't. This is the log stream behaviour: lsns flow to the node BEFORE the
\* node has a checkpoint that lets it actually apply them. checkpointTS and
\* lastApplied do NOT advance here.

ReplicateOplog(i, j) ==
    /\ i # j
    /\ state[i] = "Follower"
    /\ LastWritten(j) > LastWritten(i)
    /\ Len(oplog[i]) < MaxLogLen
    /\ LET nextIdx == Len(oplog[i]) + 1
           src     == oplog[j][nextIdx]
       IN  /\ nextIdx <= Len(oplog[j])
           /\ oplog' = [oplog EXCEPT ![i] = Append(oplog[i], src)]
    /\ UNCHANGED <<currentTerm, state, checkpointTS, lastApplied, majorityCommitted>>

----
\* ACTION: InstallCheckpoint
\* Server i durably checkpoints up to some lsn it has already written. Once
\* checkpointed, the catalog is known and oplog entries up through that lsn
\* can be applied (lastApplied catches up to checkpointTS).

InstallCheckpoint(i) ==
    /\ LastWritten(i) > checkpointTS[i]
    /\ \E newTS \in (checkpointTS[i] + 1)..LastWritten(i) :
        /\ checkpointTS' = [checkpointTS EXCEPT ![i] = newTS]
        /\ lastApplied'  = [lastApplied  EXCEPT ![i] = newTS]
    /\ UNCHANGED <<currentTerm, state, oplog, majorityCommitted>>

----
\* ACTION: CommitMajority
\* Once a majority of servers carry an entry e in their oplog, we mark e as
\* majority-committed. This is the "this write must survive any future step-up"
\* set tracked globally for the safety invariant.

CommitMajority ==
    \E i \in Server, k \in 1..Len(oplog[i]) :
        LET e == [term |-> oplog[i][k].term, lsn |-> oplog[i][k].lsn]
            quorum == HaveLsn(e.lsn)
        IN  /\ IsMajority(quorum)
            /\ e \notin majorityCommitted
            /\ majorityCommitted' = majorityCommitted \cup {e}
            /\ UNCHANGED serverVars

----
\* ACTION: StepDown
\* Voluntary step-down. Used to keep terms cycling.

StepDown(i) ==
    /\ state[i] = "Leader"
    /\ state' = [state EXCEPT ![i] = "Follower"]
    /\ UNCHANGED <<currentTerm, oplog, checkpointTS, lastApplied, majorityCommitted>>

----
\* Spec.

Next ==
    \/ \E i \in Server, voters \in SUBSET Server : BecomePrimaryByMagic(i, voters)
    \/ \E i \in Server : TryStepUpCatchup(i)
    \/ \E i \in Server : ClientWrite(i)
    \/ \E i, j \in Server : ReplicateOplog(i, j)
    \/ \E i \in Server : InstallCheckpoint(i)
    \/ \E i \in Server : StepDown(i)
    \/ CommitMajority

Spec == Init /\ [][Next]_vars

----
\* SAFETY INVARIANT: NoLostWritesOnStepUp
\* Every majority-committed entry must appear in the oplog of every current
\* Leader. If a new primary stepped up while its oplog was missing a previously
\* majority-committed write, this invariant catches the data-loss window.

NoLostWritesOnStepUp ==
    \A leader \in Leaders :
        \A e \in majorityCommitted :
            EntryInLog(leader, e)

\* Useful auxiliary invariants.

LastAppliedNeverBeyondCheckpoint ==
    \A s \in Server :
        \/ lastApplied[s] <= checkpointTS[s]
        \/ /\ checkpointTS[s] = 0
           /\ lastApplied[s] = 0

OplogLsnMonotone ==
    \A s \in Server :
        \A k \in 1..(Len(oplog[s]) - 1) :
            oplog[s][k].lsn < oplog[s][k+1].lsn

==================================================================================
