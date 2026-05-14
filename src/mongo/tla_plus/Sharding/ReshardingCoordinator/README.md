# ReshardingCoordinator TLA+ specification

Models the resharding coordinator state machine and its interaction with donor and recipient
participant services under `Stepdown`, `Stepup`, `AbortRequest`, and `InitCancelSource`. Encodes
the race named in [SERVER-115139](https://jira.mongodb.org/browse/SERVER-115139): an
`_shardsvrAbortReshardCollection` command can arrive after `Stepup` but before the participant's
`CancellationSource` is initialized, and the abort is silently lost.

## Files

- `ReshardingCoordinator.tla` &mdash; the spec (~410 lines). Actors: 1 Coordinator, 2 Donors,
  2 Recipients. State per actor: durable phase (`coordState` / `donorState` / `recipState`),
  `primary`, `hasCancelSource`, `pendingAbort`. Actions: `CoordAdvance`, `DonorAdvance`,
  `RecipAdvance`, `Stepdown`, `Stepup`, `InitCancelSource`, `AbortRequest`, `CoordEnterAborting`,
  `Commit`, plus error/done transitions.
- `MCReshardingCoordinator.{tla,cfg}` &mdash; the green configuration (`BugMode = FALSE`,
  pending aborts queue across the init gap). All invariants pass.
- `MCReshardingCoordinatorAbortBeforeInit.cfg` &mdash; the bug configuration
  (`BugMode = TRUE`). TLC produces a counterexample for `AbortAlwaysHandled` showing the
  Stepup &rarr; AbortRequest &rarr; InitCancelSource trace where the abort is dropped.

## Invariants

- `AbortAlwaysHandled` &mdash; if an abort has been requested, it must either be observed or
  queued in `pendingAbort` on some actor.
- `NoCommitAfterAbort` &mdash; the coordinator does not reach `CoordDone` once an abort has been
  requested.
- `EveryStepupInitializesCancelSource` &mdash; on the success path every actor has a live
  `CancellationSource`.
- `CoordMonotoneOnSuccess`, `ParticipantsRespectCoordinator`,
  `StepdownDestroysCancelSource` &mdash; structural sanity.

## Running the model checker

```
cd src/mongo/tla_plus
./download-tlc.sh
./model-check.sh Sharding/ReshardingCoordinator
```

The script picks up `MCReshardingCoordinator.cfg` by default. To check the bug cfg, copy it over
or rename it (or run TLC directly):

```
cp Sharding/ReshardingCoordinator/MCReshardingCoordinatorAbortBeforeInit.cfg \
   Sharding/ReshardingCoordinator/MCReshardingCoordinator.cfg
./model-check.sh Sharding/ReshardingCoordinator
```
