\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE CrossDBRenameWriterFence -----------------------------------------
\* This specification models the locking schedule of a cross-database renameCollection on a replica
\* set (SERVER-101428). Renaming a collection across databases is implemented in three sequential
\* phases on a single node:
\*
\*  P1 (clone):  acquire MODE_S on `source` and MODE_X on a temporary collection `tmp` in the target
\*               database; copy every document from `source` to `tmp`; release both locks.
\*  P2 (rename): rename `tmp` to `target` under MODE_X.
\*  P3 (drop):   drop `source` under MODE_X.
\*
\* The bug: in the production code path (src/mongo/db/shard_role/shard_catalog/rename_collection.cpp
\* ~line 991), the MODE_S lock on `source` is released at the end of P1, *before* P2/P3 execute.
\* During the unlocked window between P1 and P3 a writer can take MODE_IX on `source` and insert a
\* document. That insert hits the original source collection, which P3 then drops -- the write is
\* neither in the renamed (target) collection nor in any surviving collection. It is lost.
\*
\* This spec models the lock schedule, a single writer racing the rename, and the resulting document
\* set. The correctness invariant `NoWriteLost' asserts that every write that the client observed as
\* acknowledged is readable from *some* collection after the rename completes.
\*
\* Two CONSTANTS toggle the bug vs. the fix:
\*  - HoldSourceLockUntilRename = FALSE  -> models the buggy production schedule
\*  - HoldSourceLockUntilRename = TRUE   -> models the proposed fix (keep MODE_S/MODE_X on source
\*                                          until after P3)
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Catalog/CrossDBRenameWriterFence              \* uses MC*.cfg (bug cfg)
\*
\* See MCCrossDBRenameWriterFence_green.cfg for the fixed-schedule verification run.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Writes,                          \* Finite set of writes the racing writer may attempt, e.g. {w1,w2}.
    HoldSourceLockUntilRename        \* BOOLEAN. TRUE => fix; FALSE => SERVER-101428 schedule.

ASSUME Cardinality(Writes) >= 1
ASSUME HoldSourceLockUntilRename \in BOOLEAN

\* Lock modes we care about. MODE_S blocks IX; MODE_X blocks everything; MODE_NONE blocks nothing.
LockNone == "NONE"
LockS    == "S"
LockIX   == "IX"
LockX    == "X"

\* Phases of the cross-DB rename.
RenameIdle    == "idle"
RenameClone   == "clone"     \* P1: holding S on source, X on tmp, copying docs.
RenameUnlocked== "unlocked"  \* Buggy window: source S lock released, rename not yet executed.
RenameRename  == "rename"    \* P2: rename tmp -> target under X on tmp/target.
RenameDrop    == "drop"      \* P3: drop source under X on source.
RenameDone    == "done"

(* ----- State variables -------------------------------------------------------------------------- *)
VARIABLE renamePhase           \* Current phase of the rename coordinator.
VARIABLE sourceLock            \* Lock currently held on the `source' namespace by the renamer.
VARIABLE writerPhase           \* For each w in Writes: "pending" | "acked_source" | "blocked" | "rejected".
VARIABLE sourceDocs            \* Set of writes durably present in the `source' collection.
VARIABLE tmpDocs               \* Set of writes durably present in the `tmp' collection.
VARIABLE targetDocs            \* Set of writes durably present in the `target' collection after rename.
VARIABLE sourceDropped         \* TRUE once P3 has dropped the original `source' collection.
VARIABLE clientAcked           \* Writes the client received OK for. Every one of these MUST be
                               \* readable from some surviving collection at the end of the run.

vars == << renamePhase, sourceLock, writerPhase, sourceDocs, tmpDocs, targetDocs,
           sourceDropped, clientAcked >>

WriterPhases == {"pending", "acked_source", "blocked", "rejected"}

(* ----- Initial state ---------------------------------------------------------------------------- *)
Init ==
    /\ renamePhase   = RenameIdle
    /\ sourceLock    = LockNone
    /\ writerPhase   = [w \in Writes |-> "pending"]
    /\ sourceDocs    = {}     \* Source starts empty for clarity; the lost-write bug is independent
                              \* of pre-existing content.
    /\ tmpDocs       = {}
    /\ targetDocs    = {}
    /\ sourceDropped = FALSE
    /\ clientAcked   = {}

(* ----- Rename coordinator transitions ----------------------------------------------------------- *)

\* P1 begin: acquire MODE_S on source, MODE_X on tmp, start cloning.
\* Cannot start while a writer holds IX on source.
RenameStartClone ==
    /\ renamePhase = RenameIdle
    /\ sourceLock \in {LockNone}
    /\ renamePhase' = RenameClone
    /\ sourceLock'  = LockS
    /\ tmpDocs'     = sourceDocs              \* Clone copies the current docs into tmp.
    /\ UNCHANGED << writerPhase, sourceDocs, targetDocs, sourceDropped, clientAcked >>

\* End of P1: release source lock if running the buggy schedule, else hold it.
\*
\* In the buggy schedule the source MODE_S lock is released *here* (line 991 of
\* rename_collection.cpp) before P2/P3. In the fixed schedule we keep the source locked (escalated to
\* MODE_X in preparation for the drop) for the duration of P2+P3.
RenameFinishClone ==
    /\ renamePhase = RenameClone
    /\ IF HoldSourceLockUntilRename
        THEN /\ renamePhase' = RenameRename
             /\ sourceLock'  = LockX           \* Escalate to MODE_X; writers stay blocked.
        ELSE /\ renamePhase' = RenameUnlocked  \* Buggy window: source momentarily lockless.
             /\ sourceLock'  = LockNone
    /\ UNCHANGED << writerPhase, sourceDocs, tmpDocs, targetDocs, sourceDropped, clientAcked >>

\* Acquire the cross-namespace X lock needed to execute the rename. From the unlocked window the
\* renamer must re-acquire source/tmp/target locks.
RenameAcquireForRename ==
    /\ renamePhase = RenameUnlocked
    /\ sourceLock = LockNone                  \* No writer currently holds IX (writer either finished
                                              \* or hasn't started); modelled as a serial step.
    /\ renamePhase' = RenameRename
    /\ sourceLock'  = LockX
    /\ UNCHANGED << writerPhase, sourceDocs, tmpDocs, targetDocs, sourceDropped, clientAcked >>

\* P2: rename tmp -> target. The tmp collection's contents become target.
RenameDoRename ==
    /\ renamePhase = RenameRename
    /\ renamePhase' = RenameDrop
    /\ targetDocs'  = tmpDocs
    /\ tmpDocs'     = {}
    /\ UNCHANGED << sourceLock, writerPhase, sourceDocs, sourceDropped, clientAcked >>

\* P3: drop the original `source` collection. Any docs still in source are now unreachable.
RenameDoDrop ==
    /\ renamePhase = RenameDrop
    /\ renamePhase' = RenameDone
    /\ sourceLock'  = LockNone
    /\ sourceDocs'  = {}
    /\ sourceDropped' = TRUE
    /\ UNCHANGED << writerPhase, tmpDocs, targetDocs, clientAcked >>

(* ----- Writer transitions ----------------------------------------------------------------------- *)

\* A writer wants MODE_IX on source. It can proceed only if:
\*   - source is not under MODE_S or MODE_X from the renamer, AND
\*   - source has not been dropped (otherwise the namespace would be NamespaceNotFound).
\*
\* If source is under S or X, we model the writer as blocked (waiting); for the purposes of the
\* lost-write bug, we let the renamer drive forward and the writer eventually retries. To keep the
\* state space finite, a blocked writer simply cannot acknowledge until it acquires the lock.
\*
\* If source has been dropped, the insert returns NamespaceNotFound; the client knows the write was
\* not accepted, so it is NOT added to clientAcked (no contract violation).
WriterTryInsert(w) ==
    /\ writerPhase[w] = "pending"
    /\ \/ /\ sourceLock \in {LockS, LockX}
          /\ writerPhase' = [writerPhase EXCEPT ![w] = "blocked"]
          /\ UNCHANGED << renamePhase, sourceLock, sourceDocs, tmpDocs, targetDocs,
                          sourceDropped, clientAcked >>
       \/ /\ sourceLock = LockNone
          /\ sourceDropped
          \* Source dropped: the insert is rejected (NamespaceNotFound). No ack, no doc.
          /\ writerPhase' = [writerPhase EXCEPT ![w] = "rejected"]
          /\ UNCHANGED << renamePhase, sourceLock, sourceDocs, tmpDocs, targetDocs,
                          sourceDropped, clientAcked >>
       \/ /\ sourceLock = LockNone
          /\ ~sourceDropped
          \* Source unlocked and alive: writer takes IX, inserts, gets acked.
          /\ writerPhase' = [writerPhase EXCEPT ![w] = "acked_source"]
          /\ sourceDocs' = sourceDocs \cup {w}
          /\ clientAcked' = clientAcked \cup {w}
          /\ UNCHANGED << renamePhase, sourceLock, tmpDocs, targetDocs, sourceDropped >>

\* A blocked writer retries once the lock clears. We collapse the retry into a single step that
\* re-runs WriterTryInsert by flipping back to "pending"; this keeps the spec tractable.
WriterRetry(w) ==
    /\ writerPhase[w] = "blocked"
    /\ sourceLock = LockNone
    /\ writerPhase' = [writerPhase EXCEPT ![w] = "pending"]
    /\ UNCHANGED << renamePhase, sourceLock, sourceDocs, tmpDocs, targetDocs, sourceDropped,
                    clientAcked >>

(* ----- Terminal stutter to keep TLC happy with a finite state space ----------------------------- *)

\* When the rename has completed and every writer has reached a terminal phase, the spec
\* deliberately stops doing useful work. Without an explicit stutter step TLC would flag this as a
\* spurious deadlock; this action allows the system to remain in its terminal state.
TerminalStutter ==
    /\ renamePhase = RenameDone
    /\ \A w \in Writes : writerPhase[w] \in {"acked_source", "rejected"}
    /\ UNCHANGED vars

(* ----- Next-state relation ---------------------------------------------------------------------- *)

Next ==
    \/ RenameStartClone
    \/ RenameFinishClone
    \/ RenameAcquireForRename
    \/ RenameDoRename
    \/ RenameDoDrop
    \/ \E w \in Writes : WriterTryInsert(w)
    \/ \E w \in Writes : WriterRetry(w)
    \/ TerminalStutter

Fairness ==
    /\ WF_vars(RenameStartClone)
    /\ WF_vars(RenameFinishClone)
    /\ WF_vars(RenameAcquireForRename)
    /\ WF_vars(RenameDoRename)
    /\ WF_vars(RenameDoDrop)
    /\ \A w \in Writes : WF_vars(WriterTryInsert(w))
    /\ \A w \in Writes : WF_vars(WriterRetry(w))

Spec == Init /\ [][Next]_vars /\ Fairness

(* ----- Type invariants -------------------------------------------------------------------------- *)

RenamePhases == {RenameIdle, RenameClone, RenameUnlocked, RenameRename, RenameDrop, RenameDone}
LockModes    == {LockNone, LockS, LockIX, LockX}

TypeOK ==
    /\ renamePhase   \in RenamePhases
    /\ sourceLock    \in LockModes
    /\ writerPhase   \in [Writes -> WriterPhases]
    /\ sourceDocs    \subseteq Writes
    /\ tmpDocs       \subseteq Writes
    /\ targetDocs    \subseteq Writes
    /\ sourceDropped \in BOOLEAN
    /\ clientAcked   \subseteq Writes

(* ----- Correctness properties ------------------------------------------------------------------- *)

\* Lock exclusivity: while the renamer holds MODE_S or MODE_X on source, no writer is mid-insert.
\* (Trivially true in this spec by construction, but documents the lock semantics.)
LockExclusivity ==
    sourceLock \in {LockS, LockX} =>
        \A w \in Writes : writerPhase[w] # "acked_source" \/ w \in sourceDocs

\* Headline invariant. Every write the client observed as acknowledged must, at every step,
\* be readable from some collection in the system. A write disappears iff it landed in `source`
\* during the unlocked P1->P2 window and then P3 dropped `source`.
NoWriteLost ==
    \A w \in clientAcked :
        \/ w \in sourceDocs
        \/ w \in tmpDocs
        \/ w \in targetDocs

\* Stronger end-state form: once the rename completes, every acked write is in `target` or still
\* in `source` (the latter only possible if the writer raced *after* P3, which cannot happen since
\* source is dropped). This is the form that fails dramatically under the bug.
NoWriteLostAtEnd ==
    renamePhase = RenameDone =>
        \A w \in clientAcked :
            \/ w \in targetDocs
            \/ w \in sourceDocs

(* ----- Liveness --------------------------------------------------------------------------------- *)

\* Eventually every writer either gets acked or is rejected by NamespaceNotFound (post-drop). No
\* writer is stuck forever in the "blocked" state.
WriterMakesProgress ==
    \A w \in Writes :
        <>(writerPhase[w] \in {"acked_source", "rejected"})

\* The rename eventually completes.
RenameTerminates == <>(renamePhase = RenameDone)
====================================================================================================
