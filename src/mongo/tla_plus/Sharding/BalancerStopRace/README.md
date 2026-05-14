# BalancerStopRace

Formal model of the race between `_configsvrBalancerStop` and the migration commit path on a
config-server replica set that may step down mid-round. Tracks [SERVER-100155][ticket]
(CAR-impact 5).

## The bug, in one paragraph

`_configsvrBalancerStop` calls `Balancer::joinCurrentRound`, which waits for
`!_inBalancerRound`. When a stepdown fires mid-round, `requestTermination` kills the balancer's
`OperationContext`; the round-loop catches `InterruptedDueToReplStateChange` and runs
`_endRound`, clearing `_inBalancerRound`. `joinCurrentRound` observes a "clean" state and
returns OK -- but the migration command that was dispatched to the recipient shard before the
interrupt was never durably cancelled. The recipient (or migration coordinator recovery on the
new primary) may finalize the commit *after* `balancerStop` returned OK, breaking every
operator workflow that runs `mongodump` / `mongosync` after stopping the balancer.

## What this spec models

Three composed actors:

| Actor                  | Variables                                                                 |
|------------------------|---------------------------------------------------------------------------|
| Balancer coordinator   | `balancerMode`, `inBalancerRound`, `numBalancerRounds`, `threadState`, `threadInterrupted` |
| Config-server stepdown | `StepDown`, `ThreadTerminate`                                             |
| Migration commit path  | `migrationState` (fixed-arity over `1..MaxMigrations`)                    |
| Operator               | `stopRequested`, `stopReturnedOk`, `stopCallsIssued`                      |

Load-bearing invariant `StopThenObserveStableState`: once `balancerStop` has returned OK, no
migration may transition into `committed`. Ghost counter `commitsAfterStopOk` ticks on every
post-OK commit; the invariant pins it at zero.

## Bug toggle

`AllowStepdownInterleave \in BOOLEAN`. When `TRUE`, the interrupted-round end clears
`_inBalancerRound` without draining in-flight commits -- master-branch behaviour as of the
ticket. When `FALSE`, the coordinator must observe `NoInFlight` before declaring the round
complete, even on the interrupt path.

## Running

```
cd src/mongo/tla_plus
./model-check.sh Sharding/BalancerStopRace
```

`MCBalancerStopRace.cfg` is the green run (`AllowStepdownInterleave = FALSE`): every safety
invariant holds (197 distinct states, depth 15). To reproduce the bug, pass
`-config MCBalancerStopRace_Bug.cfg` to TLC; the model-checker fails
`StopThenObserveStableState` in 8 states and prints the
`BeginRound -> DispatchMigration -> StepDown -> EndRoundOnInterrupt -> BalancerStopBegin ->
BalancerStopReturnOk -> ShardCommitsMigration` counterexample.

[ticket]: https://jira.mongodb.org/browse/SERVER-100155
