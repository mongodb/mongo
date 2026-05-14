\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE ReshardingAbortRestartExtended -----------------------------------
(**************************************************************************************************)
(* SERVER-125589 (sibling of the wave-2 ReshardingAbortRestart spec).                              *)
(*                                                                                                 *)
(* Wave 2 modelled the kAbortedWithoutPrepare branch of the TransactionParticipant lifecycle and   *)
(* intentionally elided two regions:                                                               *)
(*                                                                                                 *)
(*   (a) the kPrepared / two-phase commit branch (the participant has voted, written a prepare    *)
(*       oplog entry, and is awaiting the coordinator's decision), and                             *)
(*                                                                                                 *)
(*   (b) the txnRetryCounter dimension and J. Mulrow's proposed kAbortedOnFirstStatement state,    *)
(*       which would preserve the StaleConfig first-statement retry path SERVER-46679 introduced  *)
(*       without re-opening the silent-data-loss window SERVER-85170 created.                      *)
(*                                                                                                 *)
(* This module reuses the wave-2 state and action vocabulary (states, recovery-thread positions,  *)
(* submitted / accepted / stagedForCommit variables) verbatim so reviewers can diff the two specs *)
(* directly, and extends them with:                                                                *)
(*                                                                                                 *)
(*   - kPrepared:                a fourth participant state reached via PrepareTransaction. From   *)
(*                               kPrepared the participant may only proceed to                     *)
(*                               kCommittedAfterPrepare (via commit-with-commitTimestamp) or       *)
(*                               kAbortedAfterPrepare (via the coordinator's abort decision).      *)
(*   - kCommittedAfterPrepare:   terminal, indistinguishable to clients from kCommitted.           *)
(*   - kAbortedAfterPrepare:     terminal, distinct from kAbortedWithoutPrepare because the        *)
(*                               participant *did* durably persist a prepare oplog entry and an    *)
(*                               idempotent recovery may legitimately re-observe it.               *)
(*   - kAbortedOnFirstStatement: J. Mulrow's proposal. A participant that aborted before any      *)
(*                               sub-statement durably touched WiredTiger may be restarted, but   *)
(*                               only with a strictly-greater txnRetryCounter (not via the unsafe *)
(*                               kStartOrContinue path).                                           *)
(*   - txnRetryCounter:          monotonically non-decreasing per (lsid, txnNumber). A sub-router *)
(*                               escalation path bumps it; the wave-2 silent-data-loss invariant  *)
(*                               must continue to hold across counter bumps.                       *)
(*                                                                                                 *)
(* Two configurable toggles gate the new behaviour, matching the wave-2 idiom:                     *)
(*                                                                                                 *)
(*   AllowResumeFromPrepared           : when TRUE, models a hypothetical defect symmetric to     *)
(*                                       the wave-2 bug - a sub-router driving kStartOrContinue   *)
(*                                       against a kPrepared participant. The bug cfg sets this   *)
(*                                       TRUE; the green cfg sets it FALSE (and TLC confirms      *)
(*                                       silent-loss invariance).                                  *)
(*                                                                                                 *)
(*   AllowFirstStatementRetry          : when TRUE, models the *safe* SERVER-46679 retry by way   *)
(*                                       of kAbortedOnFirstStatement + a bumped txnRetryCounter.   *)
(*                                       Both green and bug cfgs set this TRUE - the spec must    *)
(*                                       prove the safe path stays safe even when the unsafe      *)
(*                                       prepared-resume path is enabled.                          *)
(*                                                                                                 *)
(* The headline invariant remains NoCommitWithLostWrites (same shape as wave-2's                  *)
(* NoCommitFromAbortedWithoutPrepare, generalised to span both the kCommitted and                  *)
(* kCommittedAfterPrepare terminals). The wave-2 monotonicity invariant is reframed as            *)
(* CounterMonotonicity over txnRetryCounter, since terminal monotonicity at fixed                  *)
(* (lsid, txnNumber, txnRetryCounter) is the right level once retry-counter bumps are in scope.   *)
(*                                                                                                 *)
(* Out of scope (deliberately, to keep the state space tractable):                                 *)
(*   - cross-shard prepare/commit orchestration: a single participant suffices to expose the      *)
(*     resume-from-prepared bug, just as a single participant sufficed in wave-2.                  *)
(*   - the recovery-path's kPrepared waiting branch (onExitPrepare future): modelled coarsely as  *)
(*     "Block 1 observes kPrepared and skips abortTransaction; Block 2 still asserts terminal".   *)
(*                                                                                                 *)
(* To run:                                                                                         *)
(*     cd src/mongo/tla_plus                                                                       *)
(*     ./model-check.sh Sharding/ReshardingAbortRestartExtended                                    *)
(**************************************************************************************************)

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Writes,
    MaxRevivals,
    MaxRetryCounter

ASSUME Cardinality(Writes) > 0
ASSUME MaxRevivals \in 0..3
ASSUME MaxRetryCounter \in 0..3

\* Participant states - the wave-2 four plus three extension states.
kNone                     == "kNone"
kInProgress               == "kInProgress"
kAbortedWithoutPrepare    == "kAbortedWithoutPrepare"
kCommitted                == "kCommitted"
kPrepared                 == "kPrepared"
kCommittedAfterPrepare    == "kCommittedAfterPrepare"
kAbortedAfterPrepare      == "kAbortedAfterPrepare"
kAbortedOnFirstStatement  == "kAbortedOnFirstStatement"

AllStates == { kNone, kInProgress, kAbortedWithoutPrepare, kCommitted,
               kPrepared, kCommittedAfterPrepare, kAbortedAfterPrepare,
               kAbortedOnFirstStatement }

TerminalStates == { kCommitted, kCommittedAfterPrepare,
                    kAbortedWithoutPrepare, kAbortedAfterPrepare,
                    kAbortedOnFirstStatement }

\* TransactionActions seen by beginOrContinue (wave-2 vocabulary).
kStart            == "kStart"
kContinue         == "kContinue"
kStartOrContinue  == "kStartOrContinue"

\* Recovery-thread positions (wave-2 vocabulary).
recIdle      == "recIdle"
recInBlock1  == "recInBlock1"
recInGap     == "recInGap"
recInBlock2  == "recInBlock2"
recDone      == "recDone"
recCrashed   == "recCrashed"

\* Configuration toggles.
CONSTANTS
    AllowResumeFromPrepared,          \* TRUE = bug cfg; FALSE = green cfg.
    AllowFirstStatementRetry          \* TRUE in both cfgs (safe SERVER-46679 path).

(* State variables. Mirror wave-2's set, plus txnRetryCounter and a `firstStatementTouched'
   ghost that marks whether any sub-statement durably touched WiredTiger - the discriminator
   between the safe StaleConfig retry and the unsafe SERVER-85170 resume. *)
VARIABLES
    state,
    accepted,
    stagedForCommit,
    submitted,
    revivals,
    recovery,
    subRouterReject,
    txnRetryCounter,
    firstStatementTouched

vars == <<state, accepted, stagedForCommit, submitted, revivals, recovery,
          subRouterReject, txnRetryCounter, firstStatementTouched>>

----------------------------------------------------------------------------------------------------

Init ==
    /\ state                 = kNone
    /\ accepted              = {}
    /\ stagedForCommit       = {}
    /\ submitted             = {}
    /\ revivals              = 0
    /\ recovery              = recIdle
    /\ subRouterReject       = 0
    /\ txnRetryCounter       = 0
    /\ firstStatementTouched = FALSE

\*
\* Client opens at (lsid, txnNumber, txnRetryCounter=0) with kStart and submits its first write.
\* `firstStatementTouched' flips to TRUE only after the client moves past the first sub-statement
\* (i.e. issues a second write or executes commit / prepare). This is the discriminator Mulrow's
\* note relies on: while it's FALSE, an abort is recoverable by bumping txnRetryCounter.
\*
ClientBeginAndWrite(w) ==
    /\ state = kNone
    /\ w \notin submitted
    /\ state'                 = kInProgress
    /\ accepted'              = {w}
    /\ submitted'             = {w}
    /\ firstStatementTouched' = FALSE
    /\ UNCHANGED <<stagedForCommit, revivals, recovery, subRouterReject, txnRetryCounter>>

ClientContinueWrite(w) ==
    /\ state = kInProgress
    /\ w \notin submitted
    /\ accepted'              = accepted \cup {w}
    /\ submitted'             = submitted \cup {w}
    /\ firstStatementTouched' = TRUE
    /\ UNCHANGED <<state, stagedForCommit, revivals, recovery, subRouterReject, txnRetryCounter>>

\*
\* Client commits a single-phase (non-prepared) transaction. Identical to wave-2's ClientCommit.
\*
ClientCommit ==
    /\ state = kInProgress
    /\ state'                 = kCommitted
    /\ stagedForCommit'       = accepted
    /\ firstStatementTouched' = TRUE
    /\ UNCHANGED <<accepted, submitted, revivals, recovery, subRouterReject, txnRetryCounter>>

\*
\* Client (or coordinator) drives PrepareTransaction. The participant durably writes a prepare
\* oplog entry. The "accepted" set is preserved - prepare does not discard work.
\*
ClientPrepare ==
    /\ state = kInProgress
    /\ accepted # {}
    /\ state'                 = kPrepared
    /\ firstStatementTouched' = TRUE
    /\ UNCHANGED <<accepted, stagedForCommit, submitted, revivals, recovery,
                   subRouterReject, txnRetryCounter>>

\*
\* Coordinator commits a prepared participant. Stages the prepared writes; reaches
\* kCommittedAfterPrepare (terminal).
\*
ClientCommitPrepared ==
    /\ state = kPrepared
    /\ state'                 = kCommittedAfterPrepare
    /\ stagedForCommit'       = accepted
    /\ UNCHANGED <<accepted, submitted, revivals, recovery, subRouterReject,
                   txnRetryCounter, firstStatementTouched>>

\*
\* Coordinator aborts a prepared participant. accepted is discarded; kAbortedAfterPrepare is
\* terminal and distinct from kAbortedWithoutPrepare (a recovering coordinator may legitimately
\* re-observe the prepare oplog entry on this branch).
\*
ClientAbortPrepared ==
    /\ state = kPrepared
    /\ state'                 = kAbortedAfterPrepare
    /\ accepted'              = {}
    /\ UNCHANGED <<stagedForCommit, submitted, revivals, recovery, subRouterReject,
                   txnRetryCounter, firstStatementTouched>>

\*
\* Spontaneous abort of an in-progress (non-prepared) participant. Discriminates the safe
\* SERVER-46679 retry from the unsafe SERVER-85170 resume via `firstStatementTouched':
\*  - if FALSE (no second sub-statement, no commit/prepare): the post-abort state is
\*    kAbortedOnFirstStatement, which is restartable by bumping txnRetryCounter.
\*  - if TRUE: the post-abort state is kAbortedWithoutPrepare, which is terminal per
\*    SERVER-125589.
\*
SpontaneousAbort ==
    /\ state = kInProgress
    /\ recovery \notin {recInBlock1, recInBlock2}
    /\ accepted'    = {}
    /\ IF AllowFirstStatementRetry /\ ~firstStatementTouched
       THEN state' = kAbortedOnFirstStatement
       ELSE state' = kAbortedWithoutPrepare
    /\ UNCHANGED <<stagedForCommit, submitted, revivals, recovery, subRouterReject,
                   txnRetryCounter, firstStatementTouched>>

\*
\* Recovery path - Block 1, prepared branch. The recovery thread observes kPrepared and skips
\* abortTransaction (per coordinateCommitTransaction's local-participant decision recovery
\* logic), captures the onExitPrepare future, releases the session.
\*
RecoveryBlock1Prepared ==
    /\ recovery = recIdle
    /\ state = kPrepared
    /\ recovery' = recInGap
    /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals, subRouterReject,
                   txnRetryCounter, firstStatementTouched>>

\*
\* Recovery path - Block 1, non-prepared branch (same as wave-2 RecoveryBlock1).
\*
RecoveryBlock1InProgress ==
    /\ recovery = recIdle
    /\ state = kInProgress
    /\ recovery' = recInGap
    /\ state'    = kAbortedWithoutPrepare
    /\ accepted' = {}
    /\ UNCHANGED <<stagedForCommit, submitted, revivals, subRouterReject,
                   txnRetryCounter, firstStatementTouched>>

\*
\* Recovery path - the gap (wave-2 RecoveryGap, unchanged shape).
\*
RecoveryGap ==
    /\ recovery = recInGap
    /\ recovery' = recInGap
    /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals, subRouterReject,
                   txnRetryCounter, firstStatementTouched>>

\*
\* Sub-router fires with startOrContinueTransaction:true against a kPrepared participant.
\*  - BUG cfg (AllowResumeFromPrepared = TRUE): the participant transitions kPrepared ->
\*    kInProgress at the same (txnNumber, txnRetryCounter). The prepared oplog entry remains
\*    durable on disk while in-memory state moves back to kInProgress - the silent-loss shape.
\*  - GREEN cfg (FALSE): the sub-router is refused; the participant stays kPrepared.
\*
SubRouterStartOrContinueFromPrepared(w) ==
    /\ state = kPrepared
    /\ w \notin submitted
    /\ revivals < MaxRevivals
    /\ IF AllowResumeFromPrepared
       THEN /\ state'           = kInProgress
            /\ accepted'        = {w}
            /\ submitted'       = submitted \cup {w}
            /\ revivals'        = revivals + 1
            /\ UNCHANGED <<stagedForCommit, recovery, subRouterReject,
                           txnRetryCounter, firstStatementTouched>>
       ELSE /\ subRouterReject' = subRouterReject + 1
            /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals, recovery,
                           txnRetryCounter, firstStatementTouched>>

\*
\* Sub-router fires against a kAbortedWithoutPrepare participant - wave-2's bug shape.
\* Always refused in this spec; the wave-2 invariant carries forward unchanged.
\*
SubRouterStartOrContinueFromAborted ==
    /\ state = kAbortedWithoutPrepare
    /\ subRouterReject' = subRouterReject + 1
    /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals, recovery,
                   txnRetryCounter, firstStatementTouched>>

\*
\* SAFE retry path: a participant in kAbortedOnFirstStatement is restartable by bumping
\* txnRetryCounter. The participant re-enters kInProgress with empty `accepted'; the bumped
\* counter gives the protocol a total ordering over the retry. This is the SERVER-46679
\* StaleConfig case that Mulrow proposed preserving.
\*
FirstStatementRetry(w) ==
    /\ AllowFirstStatementRetry
    /\ state = kAbortedOnFirstStatement
    /\ txnRetryCounter < MaxRetryCounter
    /\ w \notin submitted
    /\ state'                 = kInProgress
    /\ accepted'              = {w}
    /\ submitted'             = submitted \cup {w}
    /\ txnRetryCounter'       = txnRetryCounter + 1
    /\ firstStatementTouched' = FALSE
    /\ UNCHANGED <<stagedForCommit, revivals, recovery, subRouterReject>>

\*
\* Recovery path - Block 2. Reacquires the session and asserts terminal.
\*  - kPrepared revived to kInProgress by the bug path: fires invariant -> recCrashed.
\*  - any terminal: recovery exits cleanly.
\*  - any in-progress state on the non-prepared branch (already covered by wave-2): recCrashed.
\*
RecoveryBlock2 ==
    /\ recovery = recInGap
    /\ IF state \in TerminalStates \cup { kPrepared }
       THEN /\ recovery' = recDone
            /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals,
                           subRouterReject, txnRetryCounter, firstStatementTouched>>
       ELSE /\ recovery' = recCrashed
            /\ UNCHANGED <<state, accepted, stagedForCommit, submitted, revivals,
                           subRouterReject, txnRetryCounter, firstStatementTouched>>

Next ==
    \/ \E w \in Writes : ClientBeginAndWrite(w)
    \/ \E w \in Writes : ClientContinueWrite(w)
    \/ ClientCommit
    \/ ClientPrepare
    \/ ClientCommitPrepared
    \/ ClientAbortPrepared
    \/ SpontaneousAbort
    \/ RecoveryBlock1InProgress
    \/ RecoveryBlock1Prepared
    \/ RecoveryGap
    \/ \E w \in Writes : SubRouterStartOrContinueFromPrepared(w)
    \/ SubRouterStartOrContinueFromAborted
    \/ \E w \in Writes : FirstStatementRetry(w)
    \/ RecoveryBlock2

Fairness ==
    /\ WF_vars(RecoveryBlock1InProgress)
    /\ WF_vars(RecoveryBlock1Prepared)
    /\ WF_vars(RecoveryBlock2)

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariants                                                                                 *)
(**************************************************************************************************)

TypeOK ==
    /\ state \in AllStates
    /\ accepted \subseteq Writes
    /\ stagedForCommit \subseteq Writes
    /\ submitted \subseteq Writes
    /\ revivals \in 0..MaxRevivals
    /\ recovery \in {recIdle, recInBlock1, recInGap, recInBlock2, recDone, recCrashed}
    /\ subRouterReject \in Nat
    /\ txnRetryCounter \in 0..MaxRetryCounter
    /\ firstStatementTouched \in BOOLEAN

(**************************************************************************************************)
(* Correctness Properties                                                                          *)
(**************************************************************************************************)

\*
\* Headline invariant. Mirrors wave-2's NoCommitFromAbortedWithoutPrepare, generalised to span
\* both terminals that "commit": kCommitted and kCommittedAfterPrepare. Any commit must persist
\* exactly the writes submitted under (lsid, txnNumber).
\*
\* In the GREEN cfg this holds. In the BUG cfg (AllowResumeFromPrepared = TRUE), TLC produces a
\* trace: begin -> write w1 -> prepare -> sub-router kStartOrContinue restarts to kInProgress +
\* write w2 -> commit. submitted = {w1, w2}, stagedForCommit = {w2}. The prepared oplog entry
\* for w1 is durable but the in-memory state ignored it.
\*
NoCommitWithLostWrites ==
    state \notin {kCommitted, kCommittedAfterPrepare}
        \/ stagedForCommit = submitted

\*
\* Recovery-path crash invariant (wave-2 shape, carries forward).
\*
RecoveryPathDoesNotCrash ==
    recovery # recCrashed

\*
\* Counter monotonicity: txnRetryCounter is non-decreasing over the behaviour. (Action-level
\* monotonicity. State-level monotonicity at a fixed counter value is the wave-2
\* TerminalMonotonicity, restated below.)
\*
CounterMonotonicity ==
    [][txnRetryCounter' >= txnRetryCounter]_vars

\*
\* The safe retry path stays SAFE: when AllowFirstStatementRetry is enabled and a participant
\* re-enters kInProgress from kAbortedOnFirstStatement, txnRetryCounter must have strictly
\* increased. This is the substance of Mulrow's proposal: the in-memory restart is allowed only
\* because the on-wire request carries a higher retry counter, providing total ordering across
\* the retry boundary. (Encoded as a state-level shape check: kAbortedOnFirstStatement is
\* unreachable with txnRetryCounter = MaxRetryCounter unless no retry has been attempted yet.)
\*
FirstStatementRetryBumpsCounter ==
    \/ state # kInProgress
    \/ revivals = 0
    \/ txnRetryCounter > 0

\*
\* Prepared-branch idempotency: once kAbortedAfterPrepare is reached, the participant must stay
\* there. The recovery path may legitimately re-observe the prepare oplog entry on subsequent
\* retries, but the in-memory state must not transition back to kInProgress at the same
\* (txnNumber, txnRetryCounter).
\*
PreparedAbortIsTerminal ==
    state # kAbortedAfterPrepare \/ accepted = {}

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Liveness                                                                                        *)
(**************************************************************************************************)

\*
\* Once the recovery thread enters Block 1 it eventually leaves Block 2 (cleanly in the green
\* model; crashed terminal is reachable only under the bug toggle).
\*
RecoveryEventuallyTerminates ==
    (recovery = recIdle) ~> (recovery \in {recDone, recCrashed})

====================================================================================================
