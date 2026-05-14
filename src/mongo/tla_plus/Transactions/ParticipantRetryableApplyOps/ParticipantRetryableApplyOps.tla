\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/
-------------------------- MODULE ParticipantRetryableApplyOps --------------------------
\* Formal specification for SERVER-126375: ensure the Transaction Participant recognizes
\* atomic retryable applyOps entries when reconstructing per-session retryability state.
\*
\* Context: a retryable write may be expressed as either (a) one or more "normal" CRUD
\* oplog entries stamped with (lsid, txnNumber, stmtId), or (b) a single ATOMIC applyOps
\* entry stamped with (lsid, txnNumber) whose `o.applyOps' array carries a list of inner
\* operations, each with its own stmtId. The Transaction Participant on a node must, for
\* every retryable stmtId it has ever durably observed, return "already executed" when a
\* client retries the same (lsid, txnNumber, stmtId) tuple. Today the Participant walks
\* the session's oplog chain via prevOpTime and reads stmtIds out of normal entries; the
\* POC at github.com/10gen/mongo/pull/52979 extends the walker to expand the inner ops of
\* an atomic-applyOps entry, so each inner stmtId is registered.
\*
\* The spec models one shard / replica set with one session and a bounded number of
\* retryable statements. Statements arrive over time and are written either as (a) one
\* normal oplog entry per statement or (b) one atomic-applyOps entry that bundles a
\* contiguous run of statements. Failover then re-derives the Participant's
\* `executedStmtIds' from the durable oplog by following `prevOpTime' back to the start
\* of the session. The spec defines two recognizers: a LEGACY recognizer that only reads
\* stmtIds off normal entries (the pre-fix behavior), and a FIXED recognizer that also
\* expands the inner ops of atomic-applyOps entries. The legacy recognizer is expected
\* to violate the central invariant `RecognizerMatchesDurableStmtIds' whenever any
\* atomic-applyOps entry is present; the fixed recognizer is expected to preserve it.
\*
\* To run the model-checker, first edit the constants in MCParticipantRetryableApplyOps.cfg
\* if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Transactions/ParticipantRetryableApplyOps

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    StmtIds,            \* Set of statement ids that may be issued in this session.
    MAX_OPLOG_ENTRIES,  \* Cap on number of oplog entries written.
    MAX_BUNDLE          \* Max number of inner ops bundled into one atomic applyOps.

ASSUME Cardinality(StmtIds) > 0
ASSUME MAX_OPLOG_ENTRIES \in 1..100
ASSUME MAX_BUNDLE \in 1..20

\* An oplog entry is either a normal retryable write (carrying one stmtId) or an atomic
\* applyOps entry (carrying a non-empty sequence of inner stmtIds). All entries belong to
\* the same session (lsid, txnNumber) for the purposes of this spec.
NORMAL == "normal"
ATOMIC_APPLYOPS == "atomic_applyOps"

EntryKind == {NORMAL, ATOMIC_APPLYOPS}

\* A NIL prevOpTime indicates the head of the session chain.
NIL == 0

OplogEntry == [
    kind         : EntryKind,
    opTime       : Nat \ {NIL},
    prevOpTime   : Nat,
    stmtIds      : Seq(Nat) ]

(* Variables *)
VARIABLE oplog          \* Sequence of oplog entries durably written for this session.
VARIABLE nextOpTime     \* Monotone counter for opTime assignment.
VARIABLE lastOpTime     \* Tail opTime of this session's chain; NIL if empty.
VARIABLE issuedStmtIds  \* Set of stmtIds already issued (each may appear at most once
                        \* per session, mirroring server-side dedup before write).

vars == << oplog, nextOpTime, lastOpTime, issuedStmtIds >>

Init ==
    /\ oplog = << >>
    /\ nextOpTime = 1
    /\ lastOpTime = NIL
    /\ issuedStmtIds = {}

\* Set of stmtIds carried by an entry, irrespective of its kind.
EntryStmtIdSet(entry) == { entry.stmtIds[i] : i \in DOMAIN entry.stmtIds }

\* Action: primary writes one normal retryable oplog entry for a single stmtId.
WriteNormalEntry(sid) ==
    /\ Len(oplog) < MAX_OPLOG_ENTRIES
    /\ sid \in StmtIds
    /\ sid \notin issuedStmtIds
    /\ LET entry == [ kind       |-> NORMAL,
                      opTime     |-> nextOpTime,
                      prevOpTime |-> lastOpTime,
                      stmtIds    |-> << sid >> ] IN
        /\ oplog' = Append(oplog, entry)
        /\ lastOpTime' = nextOpTime
    /\ nextOpTime' = nextOpTime + 1
    /\ issuedStmtIds' = issuedStmtIds \cup {sid}

\* Action: primary writes one atomic-applyOps retryable oplog entry bundling >=1 stmtIds.
\* `bundle' is a finite injective sequence over fresh stmtIds.
WriteAtomicApplyOpsEntry(bundle) ==
    /\ Len(oplog) < MAX_OPLOG_ENTRIES
    /\ Len(bundle) >= 1
    /\ Len(bundle) <= MAX_BUNDLE
    /\ \A i \in DOMAIN bundle : bundle[i] \in StmtIds
    /\ \A i, j \in DOMAIN bundle : i # j => bundle[i] # bundle[j]
    /\ \A i \in DOMAIN bundle : bundle[i] \notin issuedStmtIds
    /\ LET entry == [ kind       |-> ATOMIC_APPLYOPS,
                      opTime     |-> nextOpTime,
                      prevOpTime |-> lastOpTime,
                      stmtIds    |-> bundle ] IN
        /\ oplog' = Append(oplog, entry)
        /\ lastOpTime' = nextOpTime
    /\ nextOpTime' = nextOpTime + 1
    /\ issuedStmtIds' = issuedStmtIds \cup EntryStmtIdSet(entry)

Next ==
    \/ \E sid \in StmtIds : WriteNormalEntry(sid)
    \/ \E bundle \in [1..MAX_BUNDLE -> StmtIds] :
            \E n \in 1..MAX_BUNDLE :
                LET truncated == [i \in 1..n |-> bundle[i]] IN
                WriteAtomicApplyOpsEntry(truncated)
    \/ ( Len(oplog) = MAX_OPLOG_ENTRIES /\ UNCHANGED vars )

Fairness ==
    /\ WF_vars(\E sid \in StmtIds : WriteNormalEntry(sid))

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------
(***************************************************************************)
(* Recognizers.                                                            *)
(*                                                                         *)
(* The Transaction Participant rebuilds executedStmtIds by walking the     *)
(* session chain backward from `lastOpTime' via `prevOpTime'. Two          *)
(* recognizers differ only in how they handle ATOMIC_APPLYOPS entries.     *)
(***************************************************************************)

\* Look up the (unique) entry with a given opTime.
EntryByOpTime(t) ==
    CHOOSE e \in { oplog[i] : i \in DOMAIN oplog } : e.opTime = t

\* Set of opTimes reachable by following prevOpTime from `lastOpTime' to NIL.
SessionChainOpTimes ==
    LET RECURSIVE Walk(_)
        Walk(t) == IF t = NIL THEN {} ELSE {t} \cup Walk(EntryByOpTime(t).prevOpTime)
    IN  Walk(lastOpTime)

\* All entries on the session chain (i.e. every oplog entry for this session).
SessionChainEntries == { EntryByOpTime(t) : t \in SessionChainOpTimes }

\* LEGACY (pre-SERVER-126375) recognizer: only reads stmtIds off normal entries; ignores
\* atomic-applyOps entries entirely. This is the behavior the POC patch corrects.
LegacyRecognizedStmtIds ==
    UNION { EntryStmtIdSet(e) : e \in { en \in SessionChainEntries : en.kind = NORMAL } }

\* FIXED recognizer: reads stmtIds off both normal entries and the inner ops of atomic
\* applyOps entries. This is the post-fix behavior.
FixedRecognizedStmtIds ==
    UNION { EntryStmtIdSet(e) : e \in SessionChainEntries }

\* Ground truth: every stmtId durably present in the oplog under this session must be
\* recognized as already-executed.
DurableStmtIds ==
    UNION { EntryStmtIdSet(oplog[i]) : i \in DOMAIN oplog }

----------------------------------------------------------------------------------------
(***************************************************************************)
(* Type invariants.                                                        *)
(***************************************************************************)

TypeOK ==
    /\ oplog \in Seq(OplogEntry)
    /\ nextOpTime \in (Nat \ {NIL})
    /\ lastOpTime \in Nat
    /\ issuedStmtIds \subseteq StmtIds

\* Each stmtId appears at most once across the whole session oplog.
StmtIdsUnique ==
    \A i, j \in DOMAIN oplog :
        \A si \in EntryStmtIdSet(oplog[i]) :
            \A sj \in EntryStmtIdSet(oplog[j]) :
                ( i # j /\ si = sj ) => FALSE

\* The session chain reaches every durably-written entry: the protocol must never lose
\* a link by mis-stamping prevOpTime.
ChainCoversAllEntries ==
    { oplog[i].opTime : i \in DOMAIN oplog } = SessionChainOpTimes

(***************************************************************************)
(* Correctness Properties.                                                 *)
(***************************************************************************)

\* THE central invariant. The fixed recognizer must reproduce exactly the set of
\* stmtIds durably present in the oplog for this session. Any divergence is a correctness
\* bug: under-recognition causes spurious re-execution of an already-applied retryable
\* stmt; over-recognition causes the Participant to falsely answer "already executed" for
\* a stmtId it has not actually applied. Symmetric difference catches both.
RecognizerMatchesDurableStmtIds ==
    FixedRecognizedStmtIds = DurableStmtIds

\* Counter-invariant: the LEGACY recognizer (pre-fix behavior) IS expected to violate
\* RecognizerMatchesDurableStmtIds whenever an atomic-applyOps entry contributed a
\* stmtId that is not also present in some normal entry. Asserting the negation here
\* makes the regression visible if someone reverts the fix and re-enables this invariant.
LegacyRecognizerMatchesDurableStmtIds ==
    LegacyRecognizedStmtIds = DurableStmtIds

\* The recognizer is monotone: once a stmtId is recognized, no later state hides it.
RecognizerMonotone ==
    [][ \A sid \in FixedRecognizedStmtIds : sid \in FixedRecognizedStmtIds' ]_vars

\* Atomic guarantee: every stmtId bundled into an atomic-applyOps entry is recognized
\* together (i.e. either all or none).
AtomicBundleAllRecognized ==
    \A i \in DOMAIN oplog :
        oplog[i].kind = ATOMIC_APPLYOPS =>
            ( \A sid \in EntryStmtIdSet(oplog[i]) : sid \in FixedRecognizedStmtIds )
        \/ ( \A sid \in EntryStmtIdSet(oplog[i]) : sid \notin FixedRecognizedStmtIds )

============================================================================
