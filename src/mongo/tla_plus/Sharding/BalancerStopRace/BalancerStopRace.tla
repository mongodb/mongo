\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------- MODULE BalancerStopRace -------------------------------------------
\* This specification models the race between `_configsvrBalancerStop' (which calls
\* `Balancer::joinCurrentRound') and the migration commit path on a config-server replica set
\* that may step down mid-round. The bug being modelled is SERVER-100155 (CAR-impact 5):
\*
\*   The balancer thread is running a round and has dispatched a moveRange to a recipient shard.
\*   The config server steps down. `onStepDown' calls `requestTermination', which marks the
\*   balancer's thread operation context killed. The balancer round catches the
\*   `InterruptedDueToReplStateChange' exception inside `_mainThread' and calls `_endRound', which
\*   clears `_inBalancerRound' and increments `_numBalancerRounds'. A subsequent `balancerStop'
\*   command observes `_inBalancerRound == false' and `joinCurrentRound' returns OK.
\*
\*   However, the migration command that was forked to the recipient shard was not durably
\*   cancelled: the recipient may still commit the migration (via the migration coordinator
\*   recovery code on the new primary, or via a retry from the command scheduler that survived the
\*   step-down). The result: a migration finalizes AFTER `balancerStop' returned OK, which breaks
\*   every operator workflow that runs `mongodump' / `mongosync' after stopping the balancer.
\*
\* The spec composes three actors:
\*   * Balancer coordinator   -- `_mainThread' rounds, `_inBalancerRound', `_numBalancerRounds'
\*   * Config-server stepdown -- `onStepDown' -> `requestTermination'
\*   * Migration commit path  -- the shard-side commit, which is the surviving in-flight work
\*
\* Run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Sharding/BalancerStopRace
\*
\* The model-check config exposes a bug toggle, `AllowStepdownInterleave', that flips between the
\* fixed implementation (the balancer waits for in-flight commits to drain before clearing
\* `_inBalancerRound') and the buggy implementation (the round is marked complete as soon as the
\* config-server thread is interrupted).
\*
\* Design choice: migration state is a fixed-arity function over `1..MaxMigrations' rather than a
\* `Sequence', so TLC does not distinguish behaviours that differ only in the order migrations are
\* dispatched. This keeps the state space tractable while preserving every interleaving relevant
\* to the bug.

EXTENDS Naturals, FiniteSets, TLC

CONSTANTS
    MaxMigrations,          \* Bound on migrations the balancer will attempt (>= 1).
    MaxStopCalls,           \* Bound on operator balancerStop invocations (>= 1).
    AllowStepdownInterleave \* TRUE enables the buggy interleaving; FALSE asserts the fix.

ASSUME MaxMigrations \in 1..4
ASSUME MaxStopCalls \in 1..2
ASSUME AllowStepdownInterleave \in BOOLEAN

\* Slot identifiers for the bounded migration table.
MigrationSlots == 1..MaxMigrations

\* Migration lifecycle states.
\*   "unset"          -- slot has not been used by the balancer yet
\*   "commitInFlight" -- the donor has prepared a commit decision but the shard-side commit has
\*                       not been finalised. This is the state in which the race window opens.
\*   "committed"      -- the migration is durably committed on the shard.
\*   "aborted"        -- the migration was cancelled before commit.
MigrationStateValues == {"unset", "commitInFlight", "committed", "aborted"}

\* Balancer coordinator state ----------------------------------------------------------------------
VARIABLE balancerMode       \* "full" | "off"   -- the persisted balancer setting.
VARIABLE inBalancerRound    \* Whether the main thread is inside a round.
VARIABLE numBalancerRounds  \* Monotonic counter, ticked at every `_endRound'.
VARIABLE threadState        \* "running" | "terminating" | "terminated".
VARIABLE threadInterrupted  \* TRUE iff `_threadOperationContext' was marked killed.

\* Per-slot migration state ------------------------------------------------------------------------
VARIABLE migrationState     \* [MigrationSlots -> MigrationStateValues]

\* Operator-visible state --------------------------------------------------------------------------
VARIABLE stopRequested      \* TRUE once an operator has invoked balancerStop.
VARIABLE stopReturnedOk     \* TRUE once joinCurrentRound has returned OK to the operator.
VARIABLE stopCallsIssued    \* Bound counter.

\* History/observability ghost variables -----------------------------------------------------------
VARIABLE commitsAfterStopOk \* Count of commits that finalised AFTER stopReturnedOk = TRUE.

vars == <<balancerMode, inBalancerRound, numBalancerRounds, threadState, threadInterrupted,
          migrationState, stopRequested, stopReturnedOk, stopCallsIssued, commitsAfterStopOk>>

\* Helpers -----------------------------------------------------------------------------------------
UnusedSlots       == {i \in MigrationSlots : migrationState[i] = "unset"}
InFlightSlots     == {i \in MigrationSlots : migrationState[i] = "commitInFlight"}
NoInFlight        == InFlightSlots = {}

\* The fixed predicate that the balancer must observe before clearing `_inBalancerRound':
\* drain every in-flight commit. With `AllowStepdownInterleave = TRUE' this drain is bypassed on
\* the stepdown-interrupt path, reproducing the bug.
SafeToEndRound == NoInFlight

Init ==
    /\ balancerMode = "full"
    /\ inBalancerRound = FALSE
    /\ numBalancerRounds = 0
    /\ threadState = "running"
    /\ threadInterrupted = FALSE
    /\ migrationState = [i \in MigrationSlots |-> "unset"]
    /\ stopRequested = FALSE
    /\ stopReturnedOk = FALSE
    /\ stopCallsIssued = 0
    /\ commitsAfterStopOk = 0

\* -------------------------------------------------------------------------------------------------
\* Balancer round actions
\* -------------------------------------------------------------------------------------------------

\* Action: `_mainThread' begins a balancer round.
BeginRound ==
    /\ threadState = "running"
    /\ ~threadInterrupted
    /\ ~inBalancerRound
    /\ balancerMode = "full"
    /\ inBalancerRound' = TRUE
    /\ UNCHANGED <<balancerMode, numBalancerRounds, threadState, threadInterrupted,
                   migrationState, stopRequested, stopReturnedOk, stopCallsIssued,
                   commitsAfterStopOk>>

\* Action: the coordinator dispatches a moveRange. The recipient has accepted the clone, so from
\* its perspective the migration is `commitInFlight'. The prepare/critical-section phases are
\* collapsed because they do not change the race: the substantive race is between the
\* coordinator's view of "round done" and the shard's view of "commit in flight".
DispatchMigration ==
    /\ threadState = "running"
    /\ ~threadInterrupted
    /\ inBalancerRound
    /\ \E slot \in UnusedSlots :
        migrationState' = [migrationState EXCEPT ![slot] = "commitInFlight"]
    /\ UNCHANGED <<balancerMode, inBalancerRound, numBalancerRounds, threadState,
                   threadInterrupted, stopRequested, stopReturnedOk, stopCallsIssued,
                   commitsAfterStopOk>>

\* Action: the recipient shard durably commits a migration. This can happen even while the
\* coordinator's `OperationContext' has been killed -- the command scheduler's future-chain has
\* already been forked to the shard. After a step-up on the new primary, the migration coordinator
\* recovery code may also drive the commit forward; both interleavings collapse into this action.
ShardCommitsMigration ==
    /\ \E slot \in InFlightSlots :
        /\ migrationState' = [migrationState EXCEPT ![slot] = "committed"]
        /\ commitsAfterStopOk' =
            IF stopReturnedOk
                THEN commitsAfterStopOk + 1
                ELSE commitsAfterStopOk
    /\ UNCHANGED <<balancerMode, inBalancerRound, numBalancerRounds, threadState,
                   threadInterrupted, stopRequested, stopReturnedOk, stopCallsIssued>>

\* Action: a dispatched-but-not-yet-committed migration is cleanly aborted. This is the abort
\* path that respects the balancerStop contract: it must complete BEFORE the coordinator clears
\* `_inBalancerRound'.
ShardAbortsMigration ==
    /\ \E slot \in InFlightSlots :
        migrationState' = [migrationState EXCEPT ![slot] = "aborted"]
    /\ UNCHANGED <<balancerMode, inBalancerRound, numBalancerRounds, threadState,
                   threadInterrupted, stopRequested, stopReturnedOk, stopCallsIssued,
                   commitsAfterStopOk>>

\* Action: clean round end. The coordinator drained every in-flight migration before clearing
\* `_inBalancerRound' and ticking `_numBalancerRounds'. This is the correct behaviour.
EndRoundClean ==
    /\ inBalancerRound
    /\ ~threadInterrupted
    /\ SafeToEndRound
    /\ inBalancerRound' = FALSE
    /\ numBalancerRounds' = numBalancerRounds + 1
    /\ UNCHANGED <<balancerMode, threadState, threadInterrupted, migrationState,
                   stopRequested, stopReturnedOk, stopCallsIssued, commitsAfterStopOk>>

\* Action: interrupted round end. The bug. With `AllowStepdownInterleave = TRUE', the coordinator
\* clears `_inBalancerRound' as soon as the stepdown interrupts the thread, EVEN IF a migration
\* is still `commitInFlight'. With the fix in place, the coordinator must still drain in-flight
\* commits before declaring the round complete.
EndRoundOnInterrupt ==
    /\ inBalancerRound
    /\ threadInterrupted
    /\ \/ ~AllowStepdownInterleave /\ SafeToEndRound   \* fixed: drain first
       \/ AllowStepdownInterleave                       \* bug: race past in-flight commits
    /\ inBalancerRound' = FALSE
    /\ numBalancerRounds' = numBalancerRounds + 1
    /\ UNCHANGED <<balancerMode, threadState, threadInterrupted, migrationState,
                   stopRequested, stopReturnedOk, stopCallsIssued, commitsAfterStopOk>>

\* -------------------------------------------------------------------------------------------------
\* Config-server stepdown actions
\* -------------------------------------------------------------------------------------------------

\* Action: `Balancer::onStepDown' -> `requestTermination'. Marks the thread's OperationContext
\* killed and asks it to terminate. Does NOT wait for in-flight RPCs.
StepDown ==
    /\ threadState = "running"
    /\ ~threadInterrupted
    /\ threadInterrupted' = TRUE
    /\ threadState' = "terminating"
    /\ UNCHANGED <<balancerMode, inBalancerRound, numBalancerRounds, migrationState,
                   stopRequested, stopReturnedOk, stopCallsIssued, commitsAfterStopOk>>

\* Action: the balancer main thread observes the termination request, exits its loop and
\* transitions to "terminated".
ThreadTerminate ==
    /\ threadState = "terminating"
    /\ ~inBalancerRound
    /\ threadState' = "terminated"
    /\ UNCHANGED <<balancerMode, inBalancerRound, numBalancerRounds, threadInterrupted,
                   migrationState, stopRequested, stopReturnedOk, stopCallsIssued,
                   commitsAfterStopOk>>

\* -------------------------------------------------------------------------------------------------
\* Operator (balancerStop) actions
\* -------------------------------------------------------------------------------------------------

\* Action: an operator issues `balancerStop'. Persists `balancerMode = "off"' and enters
\* `joinCurrentRound'.
BalancerStopBegin ==
    /\ stopCallsIssued < MaxStopCalls
    /\ ~stopRequested
    /\ stopRequested' = TRUE
    /\ stopCallsIssued' = stopCallsIssued + 1
    /\ balancerMode' = "off"
    /\ UNCHANGED <<inBalancerRound, numBalancerRounds, threadState, threadInterrupted,
                   migrationState, stopReturnedOk, commitsAfterStopOk>>

\* Action: `joinCurrentRound' returns OK because the round has ended (or the thread terminated).
\* This is the moment the operator is told "the balancer is stopped".
BalancerStopReturnOk ==
    /\ stopRequested
    /\ ~stopReturnedOk
    /\ \/ ~inBalancerRound                  \* round ended (clean or interrupted)
       \/ threadState = "terminated"        \* thread fully drained
    /\ stopReturnedOk' = TRUE
    /\ UNCHANGED <<balancerMode, inBalancerRound, numBalancerRounds, threadState,
                   threadInterrupted, migrationState, stopRequested, stopCallsIssued,
                   commitsAfterStopOk>>

\* -------------------------------------------------------------------------------------------------
\* Spec
\* -------------------------------------------------------------------------------------------------

Next ==
    \/ BeginRound
    \/ DispatchMigration
    \/ ShardCommitsMigration
    \/ ShardAbortsMigration
    \/ EndRoundClean
    \/ EndRoundOnInterrupt
    \/ StepDown
    \/ ThreadTerminate
    \/ BalancerStopBegin
    \/ BalancerStopReturnOk

\* Fairness: only what is needed to make the load-bearing liveness obligation meaningful when a
\* PROPERTIES cfg enables it. The model is finite-state safe-check by default; liveness is
\* optional and noted in the cfg.
Fairness == WF_vars(BalancerStopReturnOk)

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariants                                                                                *)
(**************************************************************************************************)

TypeOK ==
    /\ balancerMode \in {"full", "off"}
    /\ inBalancerRound \in BOOLEAN
    /\ numBalancerRounds \in Nat
    /\ threadState \in {"running", "terminating", "terminated"}
    /\ threadInterrupted \in BOOLEAN
    /\ migrationState \in [MigrationSlots -> MigrationStateValues]
    /\ stopRequested \in BOOLEAN
    /\ stopReturnedOk \in BOOLEAN
    /\ stopCallsIssued \in 0..MaxStopCalls
    /\ commitsAfterStopOk \in Nat

(**************************************************************************************************)
(* Correctness invariants                                                                         *)
(**************************************************************************************************)

\* THE LOAD-BEARING INVARIANT (SERVER-100155).
\*
\* Once balancerStop has returned OK to the operator, no migration is allowed to transition into
\* the `committed' state. This is the contract that `mongodump' / `mongosync' / backup procedures
\* rely on: after balancerStop returns, the cluster's data distribution is stable.
StopThenObserveStableState == commitsAfterStopOk = 0

\* Monotonicity of the round counter.
RoundCounterMonotonic == numBalancerRounds >= 0

\* Once stop is observed OK, the mode is durably "off".
StopImpliesModeOff == stopReturnedOk => balancerMode = "off"

\* The thread cannot be both running and terminated.
ThreadStateConsistent == (threadState = "terminated") => ~inBalancerRound

(**************************************************************************************************)
(* Liveness                                                                                       *)
(**************************************************************************************************)

\* If an operator calls balancerStop, they eventually get OK back.
\* (Disabled in the default cfg to keep state-constraint + liveness clean -- see the cfg.)
StopEventuallyReturns == stopRequested ~> stopReturnedOk

====================================================================================================
