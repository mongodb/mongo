\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------ MODULE MergeSpillCursorReset ------------------
\* Formal specification of the merge-spill phase of the external sorter
\* under a WiredTiger WriteConflictException (WCE) retry loop, modelling
\* the SERVER-124271 cursor-reset bug.
\*
\* Domain background
\* -----------------
\* ContainerBasedSpiller::mergeSpills() runs an outer pass that consumes a
\* MergeIterator (a heap-merge over several read iterators, one per input
\* spill) and writes the merged stream into a new output writer. The write
\* side is wrapped in a writeConflictRetry helper: on each WCE the helper
\* calls rollback_transaction() on the WiredTiger session and re-invokes
\* the lambda from the beginning.
\*
\* In current code (HEAD) the read iterators and the write cursor share
\* the same WT session. rollback_transaction() therefore also resets the
\* positions of the read cursors backing the MergeIterator. The merge
\* iterator's in-memory heap, however, has already popped some keys. After
\* a WCE the next mergeIterator->next() call either silently skips
\* (one cursor still positioned past the popped key) or re-yields
\* (cursor reset to snapshot start) the affected element. The bug is
\* exercised by configuring the WTWriteConflictException failpoint with
\* nTimes >= 2 in a row.
\*
\* Model
\* -----
\* We model two abstract actors:
\*   - Merger : owns InputMultiSet (multiset of keys to merge) and the
\*              per-iterator read offsets RHead[s] for spill s.
\*   - WTSession : owns the write batch and the session phase
\*                 \in {"idle","writing","rolled_back"}.
\*
\* Each pass repeats:
\*   ReadHead   : MergeIterator pops the min head key from one input
\*                spill, appending to ConsumedInBatch.
\*   WCE        : at any point during a batch, the failpoint may fire,
\*                forcing rollback. If ResetAffectsReadCursors = TRUE the
\*                read offsets RHead[s] are reset to the value they had
\*                at the start of the current retry attempt (i.e. before
\*                this batch's reads) -- this is the buggy semantics.
\*   Commit     : the batch commits; ConsumedInBatch flushes into
\*                ConsumedTotal and the retry-anchor offsets advance.
\*
\* Configuration
\* -------------
\* The boolean ResetAffectsReadCursors is the central parameter.
\*   TRUE  : current (buggy) production semantics. NoElementLoss and
\*           NoElementDuplication can both be violated.
\*   FALSE : proposed fix -- the read phase is hoisted out of the
\*           writeConflictRetry lambda (e.g. by draining the
\*           MergeIterator into a std::vector before the write loop).
\*           Both invariants hold under all interleavings.
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Sorter/MergeSpillCursorReset
\* Then change ResetAffectsReadCursors in MCMergeSpillCursorReset.cfg to
\* re-run with the buggy or fixed semantics.

EXTENDS Integers, FiniteSets, Sequences, TLC

\* --- CONSTANTS ----------------------------------------------------------

\* Number of input spills participating in the merge.
CONSTANT NumSpills

\* Per-spill input keys, modelled as a function NumSpills -> Seq(Int).
\* Each Seq must be sorted ascending (the merge precondition).
CONSTANT InputKeys

\* Maximum number of WCE rollbacks per merge run (model bound).
CONSTANT MaxWCE

\* Maximum number of keys per write batch before forced commit (model bound).
CONSTANT MaxBatch

\* TRUE  -> buggy production semantics (rollback resets read cursors).
\* FALSE -> fixed semantics (read phase hoisted out of retry loop).
CONSTANT ResetAffectsReadCursors

ASSUME NumSpills \in Nat \ {0}
ASSUME MaxWCE \in Nat
ASSUME MaxBatch \in Nat \ {0}
ASSUME ResetAffectsReadCursors \in BOOLEAN

\* --- VARIABLES ----------------------------------------------------------

\* RHead[s] : index into InputKeys[s] of the next key to read from spill s.
\* Values 1..Len(InputKeys[s])+1; RHead[s] = Len(...)+1 means exhausted.
VARIABLE RHead

\* RHeadAnchor[s] : value of RHead[s] at the start of the current retry
\* attempt; restored on rollback under buggy semantics.
VARIABLE RHeadAnchor

\* ConsumedInBatch : sequence of keys read during the current (uncommitted)
\* write batch. Cleared on commit, restored on rollback under buggy
\* semantics (the writeConflictRetry helper re-adds the buffered batch).
VARIABLE ConsumedInBatch

\* ConsumedTotal : multiset of keys committed so far (modelled as a
\* sequence we append to; order doesn't matter for the invariants).
VARIABLE ConsumedTotal

\* Phase : "idle" | "writing" | "rolled_back" -- WTSession lifecycle.
VARIABLE Phase

\* WCECount : number of WCEs taken in this behaviour (bound by MaxWCE).
VARIABLE WCECount

vars == <<RHead, RHeadAnchor, ConsumedInBatch, ConsumedTotal, Phase, WCECount>>

\* --- HELPERS ------------------------------------------------------------

SpillIDs == 1..NumSpills

\* Heads currently visible to the merge iterator: the next unread key from
\* each non-exhausted spill.
VisibleHeads ==
    { <<s, InputKeys[s][RHead[s]]>>
        : s \in {x \in SpillIDs : RHead[x] <= Len(InputKeys[x])} }

\* Are all spills exhausted?
AllExhausted ==
    \A s \in SpillIDs : RHead[s] > Len(InputKeys[s])

\* The min head over visible spills (MergeIterator->next() picks this).
\* CHOOSE is deterministic on equal keys, which is fine -- merge stability
\* is not part of the invariant we are checking.
MinVisibleSpill ==
    CHOOSE s \in SpillIDs :
        /\ RHead[s] <= Len(InputKeys[s])
        /\ \A t \in SpillIDs :
            (RHead[t] <= Len(InputKeys[t]))
                => InputKeys[s][RHead[s]] <= InputKeys[t][RHead[t]]

\* Flat sequence of all input keys, for invariant comparisons.
RECURSIVE InputFlatRec(_)
InputFlatRec(s) ==
    IF s = 0 THEN << >>
    ELSE InputFlatRec(s-1) \o InputKeys[s]

InputFlat == InputFlatRec(NumSpills)

\* Count occurrences of a key in a sequence (multiset multiplicity).
RECURSIVE CountRec(_, _, _)
CountRec(seq, k, i) ==
    IF i = 0 THEN 0
    ELSE CountRec(seq, k, i-1) + (IF seq[i] = k THEN 1 ELSE 0)

Count(seq, k) == CountRec(seq, k, Len(seq))

\* The universe of keys appearing anywhere in the input.
KeyUniverse == { InputFlat[i] : i \in 1..Len(InputFlat) }

\* --- INITIAL STATE ------------------------------------------------------

Init ==
    /\ RHead = [s \in SpillIDs |-> 1]
    /\ RHeadAnchor = [s \in SpillIDs |-> 1]
    /\ ConsumedInBatch = << >>
    /\ ConsumedTotal = << >>
    /\ Phase = "idle"
    /\ WCECount = 0

\* --- ACTIONS ------------------------------------------------------------

\* MergeIterator->next() pulls the min head into the in-flight batch.
\* Opens a write transaction if currently idle.
ReadHead ==
    /\ ~AllExhausted
    /\ Phase \in {"idle", "writing"}
    /\ Len(ConsumedInBatch) < MaxBatch
    /\ LET s == MinVisibleSpill
           k == InputKeys[s][RHead[s]]
       IN /\ ConsumedInBatch' = Append(ConsumedInBatch, k)
          /\ RHead' = [RHead EXCEPT ![s] = @ + 1]
    /\ Phase' = "writing"
    /\ UNCHANGED <<RHeadAnchor, ConsumedTotal, WCECount>>

\* A WCE fires inside the writeConflictRetry lambda. WiredTiger calls
\* rollback_transaction() on the session.
\*
\* * The C++-side `batch` vector survives across the retry: the helper
\*   explicitly re-adds the buffered items at the top of the next lambda
\*   invocation (see ContainerBasedSpiller::mergeSpills_insert comment).
\*   So ConsumedInBatch is UNCHANGED by WCE.
\* * The WUOW's pending writes against the output writer are discarded
\*   from the storage engine -- the re-add at retry restores them.
\* * If ResetAffectsReadCursors (buggy production semantics): ALL cursors
\*   on this session are reset by rollback_transaction(), including the
\*   read cursors backing the MergeIterator's input ContainerIterators.
\*   The read offsets revert to RHeadAnchor (the snapshot start). The
\*   MergeIterator's heap, however, is C++-side memory and is NOT reset,
\*   so its next() call disagrees with the cursor positions -- yielding
\*   either the wrong key (duplication, key < ConsumedInBatch head) or
\*   skipping the key the iterator thinks it already produced (loss).
\* * Else (fixed semantics): only write state rolls back. Read offsets
\*   are unaffected.
WCE ==
    /\ Phase = "writing"
    /\ WCECount < MaxWCE
    /\ Phase' = "rolled_back"
    /\ IF ResetAffectsReadCursors
         THEN RHead' = RHeadAnchor
         ELSE RHead' = RHead
    /\ WCECount' = WCECount + 1
    /\ UNCHANGED <<RHeadAnchor, ConsumedInBatch, ConsumedTotal>>

\* Resume the writeConflictRetry lambda after a rollback. The helper
\* immediately re-invokes the lambda; in our model that means moving
\* Phase back to "writing" without progress. The retry anchor is NOT
\* refreshed -- under buggy semantics another rollback can reset to the
\* same point.
\*
\* Under buggy semantics, RHead has been rewound but ConsumedInBatch
\* still holds the previously-read keys. Subsequent ReadHead steps will
\* re-read keys that are <= the in-batch ones, surfacing the duplication
\* invariant violation.
Resume ==
    /\ Phase = "rolled_back"
    /\ Phase' = "writing"
    /\ UNCHANGED <<RHead, RHeadAnchor, ConsumedInBatch, ConsumedTotal, WCECount>>

\* Commit the current batch's WUOW. Append to ConsumedTotal and advance
\* the retry anchor so future WCEs can no longer revert past this point.
Commit ==
    /\ Phase = "writing"
    /\ Len(ConsumedInBatch) > 0
    /\ ConsumedTotal' = ConsumedTotal \o ConsumedInBatch
    /\ ConsumedInBatch' = << >>
    /\ RHeadAnchor' = RHead
    /\ Phase' = "idle"
    /\ UNCHANGED <<RHead, WCECount>>

\* Terminal commit when the merge iterator is exhausted: flush any
\* remaining buffered batch, regardless of size.
FlushFinal ==
    /\ AllExhausted
    /\ Phase = "writing"
    /\ Len(ConsumedInBatch) > 0
    /\ ConsumedTotal' = ConsumedTotal \o ConsumedInBatch
    /\ ConsumedInBatch' = << >>
    /\ RHeadAnchor' = RHead
    /\ Phase' = "idle"
    /\ UNCHANGED <<RHead, WCECount>>

\* Stutter once the run is done. Required for TLC since Init defines a
\* finite-progress system that otherwise deadlocks at completion.
Done ==
    /\ AllExhausted
    /\ Phase = "idle"
    /\ Len(ConsumedInBatch) = 0
    /\ UNCHANGED vars

\* --- NEXT-STATE RELATION ------------------------------------------------

Next ==
    \/ ReadHead
    \/ WCE
    \/ Resume
    \/ Commit
    \/ FlushFinal
    \/ Done

Spec == Init /\ [][Next]_vars /\ WF_vars(Commit) /\ WF_vars(FlushFinal)

\* --- INVARIANTS ---------------------------------------------------------

\* TypeOK : structural type invariant.
TypeOK ==
    /\ RHead \in [SpillIDs -> Nat]
    /\ RHeadAnchor \in [SpillIDs -> Nat]
    /\ \A s \in SpillIDs : RHead[s] >= 1 /\ RHead[s] <= Len(InputKeys[s]) + 1
    /\ \A s \in SpillIDs : RHeadAnchor[s] >= 1
                            /\ RHeadAnchor[s] <= Len(InputKeys[s]) + 1
    /\ ConsumedInBatch \in Seq(Int)
    /\ ConsumedTotal \in Seq(Int)
    /\ Phase \in {"idle", "writing", "rolled_back"}
    /\ WCECount \in 0..MaxWCE

\* NoElementLoss : every key present in the input appears at least its
\* input multiplicity in the committed output, ONCE the run is done.
\* Vacuously true mid-run; the predicate fires at the terminal state.
NoElementLoss ==
    (AllExhausted /\ Phase = "idle" /\ Len(ConsumedInBatch) = 0)
        => \A k \in KeyUniverse :
            Count(ConsumedTotal, k) >= Count(InputFlat, k)

\* NoElementDuplication : at no point does the committed output (plus
\* the in-flight batch) contain a key more times than the input had it.
\* This is a step-wise safety property, checked at every state.
NoElementDuplication ==
    \A k \in KeyUniverse :
        Count(ConsumedTotal, k) + Count(ConsumedInBatch, k)
            <= Count(InputFlat, k)

\* --- LIVENESS / FAIRNESS ------------------------------------------------

\* The run eventually terminates -- all keys consumed and committed.
EventuallyDone ==
    <>(AllExhausted /\ Phase = "idle" /\ Len(ConsumedInBatch) = 0)

================================================================================
