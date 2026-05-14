# RetryableInternalTxnRYOW

Formal model of the read-your-own-writes (RYOW) hazard described in
SERVER-99784: when a retryable write command is rewritten by the router as
an internal transaction with both retryable (`stmtId >= 0`) and
non-retryable (`stmtId = -1`) statements that target different shards, the
router can short-circuit the retry path the moment a shard reports the
retryable statement as already executed. If the original two-phase commit
has only landed on the shard owning the retryable statement at the time the
retry runs, the router returns success to the client without re-executing
the non-retryable statement on the second shard. A read issued in the same
session without `afterClusterTime >= commitTimestamp` then fails to see the
non-retryable write.

## Ticket

`SERVER-99784 — The short-circuiting in retryable internal transactions can
break read-your-own-writes` (Replication, P3, RYOW correctness).

## What is modeled

- one logical client session
- two routers (`mongos0`, `mongos1`)
- two shards: `shard0` owns the retryable statement (`stmtId = 0`),
  `shard1` owns the non-retryable statement (`stmtId = -1`)
- the original internal transaction transitioning each shard through
  `none -> prepared -> committed`
- a per-shard `stmtHistory` set capturing which `stmtId` values have been
  durably executed (this is what the production
  `checkStatementExecuted()` check reads)
- a retry triggered against `mongos1` after the client loses its
  connection to `mongos0` mid-commit
- the short-circuit decision made by `mongos1`, parameterized by the
  `SHORT_CIRCUIT_ON_PARTIAL_TXN` constant
- a follow-up read in the same session

The variable `writeApplied[s]` captures whether the write owned by shard
`s` is durable. `ReadYourOwnWrites` asserts that any read issued after
the client has observed a successful response sees every write that was
part of the retryable write command.

## Configurations

- `MC.cfg` (mirrored as the default `MCRetryableInternalTxnRYOW.cfg`):
  safe path, `SHORT_CIRCUIT_ON_PARTIAL_TXN = FALSE`. The router blocks
  the retry on the still-prepared participant until the original
  transaction fully commits, then short-circuits. TLC reports no
  invariant violations.
- `MC_bug.cfg`: bug path, `SHORT_CIRCUIT_ON_PARTIAL_TXN = TRUE`. The
  router declares the retry a no-op as soon as the duplicate `stmtId`
  comes back from `shard0`, regardless of `shard1`'s state. TLC
  produces a counterexample to `ReadYourOwnWrites` matching the trace
  in the ticket description.

## Running

From `src/mongo/tla_plus`:

```sh
./download-tlc.sh                                       # one time
./model-check.sh RetryableWrites/RetryableInternalTxnRYOW
```

To re-run with the bug configuration:

```sh
cp RetryableWrites/RetryableInternalTxnRYOW/MC_bug.cfg \
   RetryableWrites/RetryableInternalTxnRYOW/MCRetryableInternalTxnRYOW.cfg
./model-check.sh RetryableWrites/RetryableInternalTxnRYOW
```

The repository keeps the safe configuration checked in as the default so
CI does not produce a counterexample by design; the bug configuration
exists to reproduce the violation on demand and to gate any future fix.

## Companion test

`jstests/sharding/internal_txn_retryable_ryow.js` exercises the same
shape against a real `ShardingTest`: it writes a document, simulates the
short-circuit code path, then asserts that a follow-up read on the same
session sees the write.
