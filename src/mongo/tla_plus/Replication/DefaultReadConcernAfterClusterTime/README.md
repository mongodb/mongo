# DefaultReadConcernAfterClusterTime

Formal specification for SERVER-126299: the cluster-wide default read concern
is not honored when an explicit-session client sends `afterClusterTime`
without a `level` field.

## What the spec models

A primary maintains an oplog of strictly-increasing cluster timestamps and a
single `committedClusterTime` that advances as a majority of secondaries
replicate each entry. A client issues read requests in one of four shapes:

- `none`        — no `readConcern` field at all
- `levelOnly`   — `{readConcern: {level: "local"|"majority"}}`
- `actOnly`     — `{readConcern: {afterClusterTime: T}}` (no `level`)
- `levelAndAct` — both fields present

The pure function `ResolveLevel(shape, level)` returns the effective level
the server uses to satisfy the read. The flag `ResolveLevelFromDefault`
distinguishes the two behaviors under test:

- `TRUE`  — the fixed behavior: `actOnly` resolves to the cluster default.
- `FALSE` — the buggy behavior: `actOnly` resolves to `"local"`, ignoring
            `setDefaultRWConcern`.

`HonorClusterDefaultMajority` requires that when the cluster default is
`"majority"`, every successful read must have observed only data at or
before `committedClusterTime`.

## Configs

- `MCDefaultReadConcernAfterClusterTime.cfg` — fixed behavior; all
  invariants must hold.
- `MCDefaultReadConcernAfterClusterTime_bug.cfg` — pre-fix behavior; TLC
  must produce a counterexample to `HonorClusterDefaultMajority` showing
  an `actOnly` read observing a cluster time strictly greater than
  `committedClusterTime`.

## Running

```sh
cd src/mongo/tla_plus
./download-tlc.sh
./model-check.sh Replication/DefaultReadConcernAfterClusterTime
```

To exercise the bug cfg, copy or rename
`MCDefaultReadConcernAfterClusterTime_bug.cfg` to
`MCDefaultReadConcernAfterClusterTime.cfg` (the model-check script reads
the canonical filename).
