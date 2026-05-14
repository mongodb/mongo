# SERVER-126352 — moveChunk hang after convertToCapped

## Symptom

After `convertToCapped` is executed against a sharded collection, the next
`moveChunk` against that namespace (or, in the originally-observed reproducer,
against `config.system.sessions` while the
`convert_to_capped_unsplittable_collections.js` DSC workload is running) never
makes forward progress. The source shard's `MoveChunk` thread loops in the
session-migration phase, repeatedly emitting `moveChunk data transfer progress`
log lines (id 21993) with `sessionCatalogSourceInCatchupPhase: false`. The
client side blocks on the moveChunk admin command until either a `maxTimeMS`
deadline fires or the operation is killed manually.

The underlying parse failure surfaces as a `FailedToParse` from
`_getNextSessionMods`:

```
Missing prevOpTime field on oplog entry of previous write in transaction
```

This happens because `convertToCapped`, when batched under
`kGroupForPossiblyRetryableOperations`, can emit oplog entries inside a
batched-write block whose `prevOpTime` chain has already been cleared by
`setInitializedStatementIds` — and the session-migration scanner expects a
clean per-session `prevOpTime` chain.

The user-visible effect, regardless of the deeper batched-write cause, is a
chunk migration that never returns and a shard whose filtering-metadata view
of the namespace and whose migration-coordinator state are observably stale
relative to the rest of the cluster.

## Root cause hypothesis

The hang is a downstream consequence of the convertToCapped post-step not
fully resettling the per-collection sharding state on the data shard:

1. `convertToCapped` rewrites the collection's catalog entry and changes its
   UUID. The cached filtering metadata on the data shard still references the
   pre-conversion UUID and chunk distribution at the point control returns to
   the coordinator.
2. Any migration-coordinator document or in-progress decision artifact left
   over from a prior aborted move (or implicitly produced by the batched
   convertToCapped path) is not cleared. A subsequent `moveChunk` sees that
   document and waits for it to drain before starting its own work.
3. Because the session-migration phase of moveChunk reads through the
   leftover oplog entries produced under
   `kGroupForPossiblyRetryableOperations` — which lack a usable `prevOpTime`
   chain after the statement-id list was cleared — the source shard's
   `_getNextSessionMods` loop never advances. From the coordinator's
   perspective the migration has entered the `steady` phase and is waiting
   for the catchup queue to drain; from the source's perspective the queue
   never empties.

The net effect is that filtering metadata and migration-coordinator state are
both stale, and the moveChunk hang is the visible joint signature.

## Proposed fix

In the convertToCapped coordinator's post-step on each data shard
(`convert_to_capped_coordinator.cpp`, after the local capping operation
completes successfully):

1. **Refresh filtering metadata.** Force a `forceShardFilteringMetadataRefresh`
   against the new UUID before the coordinator releases the DDL lock. This
   ensures the next moveChunk sees a current chunk view and a current UUID,
   and that the source shard's range-deletion processor and migration source
   manager are operating against the post-conversion catalog.

2. **Abort any in-progress migration coordinator state** for the namespace.
   Reuse the existing `migrationutil::recoverMigrationCoordinations` /
   `abortAndForgetOngoingMigrations` paths that DDL coordinators already
   invoke before taking exclusive locks (e.g. drop, renameCollection). On a
   freshly-capped collection there should be no live migration, so aborting
   is either a no-op or correctly cancels a leftover coordinator document
   that would otherwise stall the next moveChunk.

3. **Drain residual session-migration artifacts.** Either skip the
   problematic insert in `OpObserverImpl::onWriteOpCompleted` when the
   statement-id list has been cleared (the narrow fix the ticket describes),
   or re-initialize the list with a single `kUninitializedStmtId` so the
   `prevOpTime` chain remains parseable. The exact shape of this change must
   go through the RSS team — the ticket's own proposal flags that an
   over-eager fix breaks unit tests in adjacent batched-write paths.

The jstest in this PR (`movechunk_after_converttocapped_no_hang.js`) is the
regression gate: it shards a collection, runs convertToCapped, then runs
moveChunk with a 60-second bounded `maxTimeMS`. On unpatched binaries it
fails with `MaxTimeMSExpired`; on a patched build it completes in under a
second.
