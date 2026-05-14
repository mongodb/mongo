---------------------------- MODULE MCBalancerStopRace --------------------------------------------
\* Model-checking harness for BalancerStopRace.tla.
\*
\* Two model configurations are provided:
\*   * MCBalancerStopRace.cfg              -- green run; `AllowStepdownInterleave = FALSE'.
\*                                            Invariants must hold.
\*   * MCBalancerStopRace_Bug.cfg          -- bug repro; `AllowStepdownInterleave = TRUE'.
\*                                            `StopThenObserveStableState' is violated and TLC
\*                                            prints the counterexample trace.

EXTENDS BalancerStopRace

\* State constraint: keep the model finite. `numBalancerRounds' is a monotone counter that
\* would let the model grow without bound otherwise; capping it to a small multiple of the
\* migration / stop budgets is enough to cover every interleaving relevant to the bug. Once a
\* commit has slipped past stop-OK we have the violation; one such witness is enough.
MaxRounds == MaxMigrations + MaxStopCalls + 2

StateConstraint ==
    /\ numBalancerRounds <= MaxRounds
    /\ commitsAfterStopOk <= 1

\* Convenience baits for trace inspection (set as INVARIANT in a debug cfg).
BaitMigrationDispatched == \A i \in MigrationSlots : migrationState[i] # "commitInFlight"
BaitMigrationCommitted  == \A i \in MigrationSlots : migrationState[i] # "committed"
BaitStepDownObserved    == ~threadInterrupted
BaitStopReturnedOk      == ~stopReturnedOk

====================================================================================================
