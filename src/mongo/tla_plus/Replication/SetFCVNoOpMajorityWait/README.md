# SetFCVNoOpMajorityWait

Formal model for **SERVER-120978**: `setFeatureCompatibilityVersion` (setFCV)
waits on a stale `opTime` when the command is a no-op (requested version
already equals actual version, e.g., the shard "prepare" phase where no
metadata is mutated).

## What this models

`set_feature_compatibility_version_command.cpp` always majority-waits on
`ReplClientInfo::getLastOp()` in an `ON_BLOCK_EXIT` (line 377). When the
command performs no writes, that stored `opTime` may predate other in-flight
FCV writes the cluster has accepted but not yet majority-committed. The
result: setFCV returns `ok:1` to the client while a concurrent prior FCV
mutation is still not durably committed.

The fix (line 392) calls `ReplClientInfo::setLastOpToSystemLastOpTime` on the
no-op path so the subsequent `waitForWriteConcern` pins the *current* system
last-applied `opTime`, which dominates every prior FCV write.

The spec abstracts:

- Majority replication of FCV-doc writes (`PersistFCVWrite`, `ReplicateFCV`,
  `AdvanceCommit`).
- Primary failover (`Stepdown`, `ElectNew`).
- The two setFCV branches: writing (`SetFCVWrite`) and no-op (`SetFCVNoOp`).
- The `waitForWriteConcern` predicate (`WaitForMajorityOf`).

## Bug toggle

`CONSTANT AllowStaleOpTimeOnNoOp`:

- `FALSE` — fixed behavior: `SetFCVNoOp` pins `SystemLastOpTime` before the
  wait. Invariant `SetFCVResultReflectsDurableState` holds.
- `TRUE` — buggy behavior: `SetFCVNoOp` waits on `clientLastOp`. TLC returns
  a counterexample.

## Invariants

- `SetFCVResultReflectsDurableState` — every recorded setFCV return must
  have waited on an `opTime` at least the system last-applied `opTime` live
  at the moment the command was processed.
- `NoOpReturnImpliesDurable` — derived corollary for no-op returns.
- `TypeOK` — structural typing.

## How to run

```sh
cd src/mongo/tla_plus
./model-check.sh Replication/SetFCVNoOpMajorityWait
```

Defaults to the green run (`AllowStaleOpTimeOnNoOp = FALSE`). To reproduce
the bug, flip the constant to `TRUE` in `MCSetFCVNoOpMajorityWait.cfg`.

## Paired empirical repro

`jstests/replsets/set_fcv_noop_returns_before_durable.js` forces a primary
step-down between the FCV-doc write and the (no-op) return, then verifies
the user-visible durability contract.
