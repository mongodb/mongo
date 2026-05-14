# SERVER-96199 design partner: FLE update to a document's shard key

## Symptom

A retryable update on an FLE2-encrypted collection that would change the
value of the document's shard-key field fails with a generic assertion
from deep inside the internal-transaction machinery. The error surfaces
as a `TransactionTooOld`-shaped or `IncompleteTransactionHistory`-shaped
failure rather than a clean, structured error that names the
combination at fault (FLE + shard-key change).

The bug was first hit by a patch build that flipped a sharded-collections
passthrough suite to use retryable writes
(`sharded_collections_jscore_passthrough_with_config_transitions_2`).

## Root cause

Two retryable-write retrofits collide:

1. **Shard-key change** uses a hand-rolled translation: when a retryable
   update would move a document across shards, mongos converts the
   update into a multi-statement transaction containing a delete on the
   source shard and an insert on the destination shard. This predates
   internal transactions. The two child writes are stamped with the
   default `stmtId = 0` because, in a standard transaction, `stmtId` is
   ignored.

2. **FLE2 CRUD** uses retryable internal transactions to atomically
   update the user document plus the ESC / ECOC bookkeeping
   collections. In an internal transaction, `stmtId` is *not* ignored:
   it is the deduplication key for retryability.

When the original write is an FLE update that *also* changes the shard
key, mongos performs the FLE re-write of the
`BatchedCommandRequest` first (`FLEUpdateOperation::serialize` /
`appendMongosRequest`). That re-write drops the
"this update may change the shard key" flag — the WCOS (write contains
owning shard) annotation never reaches the mongos dispatcher. The
dispatcher then runs the request as a vanilla retryable update, which in
turn spawns the shard-key-change translation, which in turn spawns an
internal transaction with `stmtId = 0` on both child writes. The reused
`stmtId` aborts the entire FLE internal transaction.

## Fix

Two viable paths (the ticket calls them out as alternatives; this design
recommends path A and uses path B as a same-day fallback):

### Path A (preferred): route shard-key change through internal transactions

Switch the shard-key-change codepath to use retryable internal
transactions. The scaffolding already exists behind a failpoint
(`useInternalTransactionsForShardKeyUpdates` or equivalent) — the work
was started under the internal-transactions project and never finished.
Completing it gives shard-key change the same `stmtId`-aware machinery
that FLE already uses, removing the collision by construction.

### Path B (fallback): preserve the flag through the FLE re-write

Thread the "may change shard key" bit through
`FLEUpdateOperation::serialize` and `appendMongosRequest` so the rewritten
`BatchedCommandRequest` still tells mongos the WCOS path is required.
Independently, assign distinct `stmtId`s to the delete and insert child
writes (e.g. `stmtId, stmtId + 1`) so even if the surrounding txn is an
internal one, deduplication is well-defined. This is a narrower patch
than path A and is safe to backport.

## Why the jstest accepts either outcome

A regression test that pins one specific error code would become a
liability the moment either path lands. The companion jstest
`jstests/fle2/fle_update_shard_key_field.js` asserts the *contract*: the
write either succeeds with cross-shard semantics (path A or a full fix
of path B) or fails with a clean structured error (partial path B). Both
keep the substrate honest; the generic-assertion regression is what we
refuse to ship.

## Out of scope

`findAndModify` on FLE collections that would change the shard key
follows the same code path but is gated separately by SERVER-114994
(UWE / findAndModify). That ticket subsumes the findAndModify variant of
this fix.
