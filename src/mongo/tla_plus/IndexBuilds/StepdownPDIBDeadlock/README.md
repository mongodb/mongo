# StepdownPDIBDeadlock

TLA+ model of the SERVER-126266 deadlock between stepdown and the
primary-driven index build (PDIB) coordinator. Load-bearing for the 9.0
PDIB release.

## The bug

A primary running an in-flight PDIB receives `replSetStepDown`. Stepdown
enqueues an X-mode waiter on the RSTL. RSTL is approximately fair, so any
subsequent IX request parks behind the X waiter. BackgroundSync needs IX
to apply incoming oplog entries — including the commit-quorum vote
entries the PDIB coordinator is waiting on. PDIB itself keeps holding its
own IX intent through the `voting` phase. The wait-for graph closes:

```
stepdown(X)  --waits-for-->  pdib(IX)         (RSTL queue, fair)
pdib(IX)     --waits-for-->  bgsync(IX-vote)  (commit-quorum starvation)
bgsync(IX)   --waits-for-->  stepdown(X)      (RSTL queue, fair)
```

`tryToStepDown()` then loops forever because secondaries can never catch
up to the primary's optime.

## Model layout

Three abstract threads contend on one RSTL: `PDIB`, `Stepdown`, `BgSync`.
PDIB walks a small phase machine (`idle` -> `scanning` -> `voting` ->
`committing` -> `done`, plus `aborted`). Stepdown walks (`none` ->
`enqueued` -> `holding` -> `completed`). The commit quorum is modelled
as a finite set of secondary votes that can only arrive while BgSync is
making progress.

## Bug toggle

`AllowPDIBHoldDuringStepdown` switches between configurations:

- `TRUE`  -- production behaviour; the deadlock is reachable.
- `FALSE` -- patched behaviour; PDIB observes the stepdown signal and
  releases IX via `PDIBAbortOnStepdown`. No cycle.

## Invariants and properties

- `TypeOK` -- variable domains.
- `RSTLWellFormed` -- X is exclusive; X and IX never coexist.
- `DeadlockFreedom` -- no cycle in the wait-for graph at any reachable
  state.
- `EveryStepdownCompletes` -- every enqueued stepdown eventually reaches
  `completed`.

## Run

```
cd src/mongo/tla_plus
./model-check.sh IndexBuilds/StepdownPDIBDeadlock   # green
java -cp tla2tools.jar tlc2.TLC -config \
  IndexBuilds/StepdownPDIBDeadlock/MCStepdownPDIBDeadlock_bug.cfg \
  IndexBuilds/StepdownPDIBDeadlock/MCStepdownPDIBDeadlock.tla   # bug
```

`DeadlockFreedom` and `EveryStepdownCompletes` each independently flag
the bug. `jstests/replsets/stepdown_during_pdib_no_deadlock.js`
exercises the same scenario against a real `mongod`.
