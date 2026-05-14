\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE ReshardingAbortRestart ---------------------------------------
(**************************************************************************************************)
(* SERVER-125589: Disallow kStartOrContinue restart from kAbortedWithoutPrepare to prevent silent  *)
(* data loss.                                                                                     *)
(*                                                                                                *)
(* This specification models a coordinator-side state-machine subset of the TransactionParticipant *)
(* lifecycle at a single (lsid, txnNumber). The bug surfaced first on the resharding-adjacent     *)
(* coordinator recovery path (AF-15971: the                                                       *)
(* `invariant(!txnParticipant.transactionIsOpen())' in `coordinateCommitTransaction''s            *)
(* local-participant recovery block), but the root defect is in the participant state machine.    *)
(*                                                                                                *)
(* State machine (subset; only the states needed to expose the bug):                              *)
(*                                                                                                *)
(*    kNone                                                                                       *)
(*      | beginOrContinue(kStart)                                                                 *)
(*      v                                                                                         *)
(*    kInProgress  ----------- commitTransaction --------> kCommitted                             *)
(*      |                                                                                         *)
(*      | abortTransaction                                                                        *)
(*      v                                                                                         *)
(*    kAbortedWithoutPrepare                                                                      *)
(*      | beginOrContinue(kStartOrContinue)   <----- unsafe transition (the bug)                  *)
(*      v                                                                                         *)
(*    kInProgress  (revived) ---- commitTransaction --------> kCommitted                          *)
(*                                                                                                *)
(* The unsafe transition is gated by the flag `AllowResumeFromAbortedWithoutPrepare'. The green   *)
(* model sets the flag FALSE and asserts `NoCommitFromAbortedWithoutPrepare'. The bug model sets  *)
(* the flag TRUE and TLC discovers a counterexample to the same invariant.                       *)
(*                                                                                                *)
(* The recovery thread (a coordinator-side committer) is modelled as a two-block sequence with a  *)
(* gap during which the session lock is released - faithfully matching                            *)
(* txn_two_phase_commit_cmds.cpp's recovery path. During the gap, a concurrent sub-router can     *)
(* drive the participant via kStartOrContinue.                                                    *)
(*                                                                                                *)
(* What is intentionally NOT modelled (out of scope; would explode the state space without adding *)
(* coverage for SERVER-125589):                                                                   *)
(*  - kPrepared / two-phase commit branches (the bug is in the kAbortedWithoutPrepare branch).    *)
(*  - txnRetryCounter and the StaleConfig first-statement retry (J. Mulrow's comment proposes a   *)
(*    new kAbortedOnFirstStatement state to preserve that case; out of scope here).               *)
(*  - Multiple shards / cross-shard commit. Single-participant suffices to expose silent loss.    *)
(*                                                                                                *)
(* To run the model-checker:                                                                      *)
(*     cd src/mongo/tla_plus                                                                      *)
(*     ./model-check.sh Sharding/ReshardingAbortRestart                                           *)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Writes,             \* Symbolic set of "writes" the user submits. Pre-abort vs post-restart
                        \* writes are distinguished by which epoch they were submitted in.
    MaxRevivals         \* Bound on kAbortedWithoutPrepare -> kInProgress transitions allowed by
                        \* the bug toggle. Keeps the bug model finite.

ASSUME Cardinality(Writes) > 0
ASSUME MaxRevivals \in 0..3

\* Participant states. Encoded as strings to match server-side enum names 1:1.
kNone                  == "kNone"
kInProgress            == "kInProgress"
kAbortedWithoutPrepare == "kAbortedWithoutPrepare"
kCommitted             == "kCommitted"

\* TransactionActions seen by `beginOrContinue'.
kStart            == "kStart"
kContinue         == "kContinue"
kStartOrContinue  == "kStartOrContinue"

\* Recovery-thread positions in the two-block recovery path
\* (mirrors txn_two_phase_commit_cmds.cpp recovery decision block).
recIdle      == "recIdle"          \* Recovery has not yet entered Block 1.
recInBlock1  == "recInBlock1"      \* Holds the session; observed kInProgress; about to abort.
recInGap     == "recInGap"         \* Released the session; awaiting onExitPrepare future.
recInBlock2  == "recInBlock2"      \* Re-acquired the session; about to assert terminal state.
recDone      == "recDone"          \* Recovery completed (returned to the caller).
recCrashed   == "recCrashed"       \* The recovery-path invariant fired (SIGABRT in production).

\* Bug toggle. Set FALSE in MCReshardingAbortRestart.cfg (green model), TRUE in the bug cfg.
\* When FALSE, the participant cannot leave kAbortedWithoutPrepare; sub-routers that hit an
\* aborted participant on kStartOrContinue are refused and must escalate.
\* When TRUE, the participant transitions kAbortedWithoutPrepare -> kInProgress at the same
\* txnNumber, matching the pre-fix behaviour added by SERVER-85170.
CONSTANT AllowResumeFromAbortedWithoutPrepare

(* State variables. *)
VARIABLES
    state,              \* Participant state \in {kNone, kInProgress, kAbortedWithoutPrepare,
                        \*                       kCommitted}.
    accepted,           \* Set of writes durably "accepted" by the running participant attempt -
                        \* what would survive commitTransaction. Discarded by an abort.
    stagedForCommit,    \* On commit, the set of writes that actually persist. Captured at commit
                        \* time so post-restart inspection can compare against `submitted'.
    submitted,          \* Set of writes the user has handed to the system (over the full
                        \* (lsid, txnNumber) lifetime). The "ground truth" the user expects.
    revivals,           \* Number of times the participant has been resurrected from
                        \* kAbortedWithoutPrepare back to kInProgress. Bounded by MaxRevivals.
    recovery,           \* Recovery-thread position \in {recIdle, recInBlock1, recInGap,
                        \*                              recInBlock2, recDone, recCrashed}.
    subRouterReject     \* Auxiliary: number of sub-router attempts refused because the
                        \* participant was terminal (only nonzero when the flag is FALSE).

vars == <<state, accepted, stagedForCommit, submitted, revivals, recovery, subRouterReject>>

----------------------------------------------------------------------------------------------------

Init ==
    /\ state            = kNone
    /\ accepted         = {}
    /\ stagedForCommit  = {}
    /\ submitted        = {}
    /\ revivals         = 0
    /\ recovery         = recIdle
    /\ subRouterReject  = 0

\*
\* Action: the driver opens the transaction at (lsid, txnNumber) with kStart and submits its first
\* write. Only fires once per behaviour; kStart is illegal on an active participant.
\*
ClientBeginAndWrite(w) ==
    /\ state = kNone
    /\ w \notin submitted
    /\ state'           = kInProgress
    /\ accepted'        = {w}
    /\ submitted'       = {w}
    /\ UNCHANGED <<stagedForCommit, revivals, recovery, subRouterReject>>

\*
\* Action: the driver submits a subsequent write inside the in-flight transaction (kContinue).
\* kContinue is the "ordinary" continuation action. It does NOT restart a terminal participant.
\*
ClientContinueWrite(w) ==
    /\ state = kInProgress
    /\ w \notin submitted
    /\ accepted'        = accepted \cup {w}
    /\ submitted'       = submitted \cup {w}
    /\ UNCHANGED <<state, stagedForCommit, revivals, recovery, subRouterReject>>

\*
\* Action: the driver commits the transaction. Stages `accepted' into `stagedForCommit', moves to
\* kCommitted. The participant is terminal afterwards.
\*
ClientCommit ==
    /\ state = kInProgress
    /\ state'           = kCommitted
    /\ stagedForCommit' = accepted
    /\ UNCHANGED <<accepted, submitted, revivals, recovery, subRouterReject>>

\*
\* Action: a "spontaneous" abort fires - models any cause of a participant transitioning to
\* kAbortedWithoutPrepare. Concrete instances:
\*   - explicit abortTransaction from the driver,
\*   - transactionLifetimeLimitSeconds reaper,
\*   - the recovery path's own kContinue + abortTransaction in Block 1.
\* All physically discard `accepted'.
\*
SpontaneousAbort ==
    /\ state = kInProgress
    /\ recovery \notin {recInBlock1, recInBlock2}    \* Recovery path drives its own abort below.
    /\ state'           = kAbortedWithoutPrepare
    /\ accepted'        = {}
    /\ UNCHANGED <<stagedForCommit, submitted, revivals, recovery, subRouterReject>>

\*
\* Recovery path - Block 1 (lines 400-416 in txn_two_phase_commit_cmds.cpp).
\* Enters with the session held, observes kInProgress, calls abortTransaction, captures the
\* (already-ready) onExitPrepare future, releases the session.
\*
RecoveryBlock1 ==
    /\ recovery = recIdle
    /\ state = kInProgress
    /\ recovery'        = recInGap
    /\ state'           = kAbortedWithoutPrepare
    /\ accepted'        = {}    \* abortTransaction in Block 1 discards staged work.
    /\ UNCHANGED <<stagedForCommit, submitted, revivals, subRouterReject>>

\*
\* Recovery path - the gap. Models any positive amount of time elapsing between Block 1 releasing
\* the session and Block 2 reacquiring it. The sub-router action below is the only action that
\* meaningfully advances during this gap; this action exists so `recovery' is observable as
\* recInGap by the model checker.
\*
RecoveryGap ==
    /\ recovery = recInGap
    /\ recovery'        = recInGap
    /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals, subRouterReject>>

\*
\* Sub-router fires at (lsid, txnNumber) carrying startOrContinueTransaction: true.
\* In the BUG configuration (`AllowResumeFromAbortedWithoutPrepare = TRUE'), this drives the
\* unsafe transition kAbortedWithoutPrepare -> kInProgress at the same txnNumber and submits a
\* fresh post-restart write. In the GREEN configuration, the sub-router is REFUSED - matching the
\* fix proposed in SERVER-125589 - and the participant remains terminal.
\*
SubRouterStartOrContinue(w) ==
    /\ state = kAbortedWithoutPrepare
    /\ w \notin submitted
    /\ revivals < MaxRevivals
    /\ IF AllowResumeFromAbortedWithoutPrepare
       THEN \* Unsafe path - the bug.
            /\ state'           = kInProgress
            /\ accepted'        = {w}
            /\ submitted'       = submitted \cup {w}
            /\ revivals'        = revivals + 1
            /\ UNCHANGED <<stagedForCommit, recovery, subRouterReject>>
       ELSE \* Fix path - participant refuses and the sub-router must escalate (abort + new
            \* txnNumber). We just count refusals to make the action enabled.
            /\ subRouterReject' = subRouterReject + 1
            /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals, recovery>>

\*
\* Recovery path - Block 2 (lines 421-438 in txn_two_phase_commit_cmds.cpp).
\* Reacquires the session, calls beginOrContinue(kContinue), and asserts the participant is in a
\* terminal state.
\* - If state \in {kAbortedWithoutPrepare, kCommitted}, the assertion passes - recovery exits.
\* - If state = kInProgress (revived by a sub-router during the gap), the assertion fires:
\*   `invariant(!txnParticipant.transactionIsOpen())' - SIGABRT in production.
\*
RecoveryBlock2 ==
    /\ recovery = recInGap
    /\ IF state \in {kAbortedWithoutPrepare, kCommitted}
       THEN /\ recovery'   = recDone
            /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals,
                           subRouterReject>>
       ELSE \* state = kInProgress - the recovery-path invariant fires.
            /\ recovery'   = recCrashed
            /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals,
                           subRouterReject>>

Next ==
    \/ \E w \in Writes : ClientBeginAndWrite(w)
    \/ \E w \in Writes : ClientContinueWrite(w)
    \/ ClientCommit
    \/ SpontaneousAbort
    \/ RecoveryBlock1
    \/ RecoveryGap
    \/ \E w \in Writes : SubRouterStartOrContinue(w)
    \/ RecoveryBlock2

Fairness ==
    /\ WF_vars(RecoveryBlock1)
    /\ WF_vars(RecoveryBlock2)

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariants                                                                                *)
(**************************************************************************************************)

TypeOK ==
    /\ state \in {kNone, kInProgress, kAbortedWithoutPrepare, kCommitted}
    /\ accepted \subseteq Writes
    /\ stagedForCommit \subseteq Writes
    /\ submitted \subseteq Writes
    /\ revivals \in 0..MaxRevivals
    /\ recovery \in {recIdle, recInBlock1, recInGap, recInBlock2, recDone, recCrashed}
    /\ subRouterReject \in Nat

(**************************************************************************************************)
(* Correctness Properties                                                                         *)
(**************************************************************************************************)

\*
\* The headline invariant for SERVER-125589.
\*
\* If the participant ever commits, the set of writes that persisted must equal the set of writes
\* the user submitted under (lsid, txnNumber). In other words: NO commit may persist a strict
\* subset of `submitted'. A strict subset would mean an abort silently discarded user writes and
\* a subsequent revival committed only the post-restart tail.
\*
\* In the GREEN configuration (flag FALSE), TLC explores every interleaving and this invariant
\* holds: aborts are terminal, so any commit must follow a fresh kStart from kNone (which is
\* unreachable once we are aborted).
\*
\* In the BUG configuration (flag TRUE), TLC finds a counterexample within a handful of states:
\* begin, write w1, abort, sub-router revives + submits w2, commit. submitted = {w1, w2},
\* stagedForCommit = {w2}. Silent loss of w1.
\*
NoCommitFromAbortedWithoutPrepare ==
    state # kCommitted \/ stagedForCommit = submitted

\*
\* The recovery-path invariant. When the recovery thread reaches Block 2 it must observe a
\* terminal participant; otherwise the in-process `invariant(!txnParticipant.transactionIsOpen())'
\* fires and the mongod process aborts.
\*
\* Surfaces the crash-shaped symptom (AF-15971) alongside the silent-data-loss invariant. In the
\* green model `recovery' never reaches `recCrashed'.
\*
RecoveryPathDoesNotCrash ==
    recovery # recCrashed

\*
\* Monotonicity of the participant state at a fixed (lsid, txnNumber) - the pre-2024 protocol
\* invariant called out in the Jira description. Once terminal, the participant stays terminal.
\* SERVER-85170 broke this. The fix in SERVER-125589 restores it.
\*
\* This is a *current-state* shape check: `revivals' counts how many times the participant has
\* been resurrected from kAbortedWithoutPrepare. Under the fix, revivals stays 0.
\*
TerminalMonotonicity ==
    revivals = 0

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Liveness                                                                                       *)
(**************************************************************************************************)

\*
\* The recovery thread, once it enters Block 1, eventually leaves Block 2 (either cleanly or via
\* the crashed terminal). In the green model the only terminal is `recDone'.
\*
RecoveryEventuallyTerminates ==
    (recovery = recIdle) ~> (recovery \in {recDone, recCrashed})

====================================================================================================
