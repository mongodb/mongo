# FCVInitDisaggLSN

Sibling spec to `RaftMongoReplTimestamp`. Scoped to the FCV-init handshake
on a node backed by disaggregated storage. Fingers the bug under
investigation in SERVER-112445.

## The bug

FCV init on a disagg node moves through `LoadFCVDoc` (priming local stable
timestamp from the recovery oplog) and `OpenLogService` (opening the
log-service handle and observing its starting LSN). Between these two
steps the local stable timestamp and the log-service LSN disagree. The
original code path returned success to `setFCV` before that window
closed; a subsequent stepdown or restart could observe either watermark
in isolation, producing rollback or recovery decisions inconsistent with
the other tier. This spec models that window and the fix.

## Layout

- `FCVInitDisaggLSN.tla` - five-phase state machine
  (`Idle` → `LoadFCVDoc` → `OpenLogService` → `WaitConvergence` →
  `Returned`), two LSN streams (`localStableTS`, `logServiceLSN`), one
  pinned watermark (`fcvDocCommitLSN`), plus a public `returnTrace`.
- `MCFCVInitDisaggLSN.tla` - state constraint and symmetry.
- `MCFCVInitDisaggLSN.cfg` - green. `AllowReturnBeforeConvergence
  = FALSE`. Invariants and liveness hold.
- `MCFCVInitDisaggLSN.bug.cfg` - bug. `AllowReturnBeforeConvergence
  = TRUE`. `BootstrapCoherence` and `ReturnTraceCoherence` are violated
  by `ReturnEarly`. Exercise via `cp MCFCVInitDisaggLSN.bug.cfg
  MCFCVInitDisaggLSN.cfg`.

## Headline invariant

`BootstrapCoherence`: when `fcvPhase[s] = "Returned"`,
`logServiceLSN[s] = localStableTS[s]` and both are at or above
`fcvDocCommitLSN[s]`. The fix makes `ReturnConverged` - the only
`Returned`-bound action - guard on those equalities.

## Bug toggle

`AllowReturnBeforeConvergence` enables `ReturnEarly`
(`WaitConvergence → Returned` while `logServiceLSN # localStableTS`).
TLC counterexample length is 5:
`Init → BeginInit → LoadFCVDoc → OpenLogService → ReturnEarly`. The bug
fires on the first init and needs no replication-protocol interaction.

## Running

```
cd src/mongo/tla_plus
./model-check.sh Replication/FCVInitDisaggLSN
```

That picks up `MCFCVInitDisaggLSN.cfg`. For the bug config, copy
`MCFCVInitDisaggLSN.bug.cfg` over it and re-run.

## Relationship to RaftMongoReplTimestamp

The sibling covers cluster-wide timestamp machinery. This spec is
orthogonal: single-node behavior reproduces the bug. Read
`localStableTS` here as a per-node projection of `committedSnapshot`
there.
