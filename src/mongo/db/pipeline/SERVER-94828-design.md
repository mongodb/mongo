# SERVER-94828 — UUID-gated cleanup of `$out` temp buckets

## Symptom

When `$out` writes to a time-series collection and the operation is interrupted (kill,
stepdown, client disconnect) after the temporary rename but before the user-facing view is
created (`OutStage::doDispose` enters the `kRenameComplete` branch), the cleanup path calls
`pExpCtx->getMongoProcessInterface()->dropTempCollection(opCtx, _outputNs.makeTimeseriesBucketsNamespace())`.

This drop targets the buckets collection **by name**. If a concurrent client has dropped
the user-facing collection and re-created a fresh time-series collection under the same
name in the cleanup window, the cleanup destructor will drop an unrelated buckets
collection that the failing `$out` never owned. That is data loss. The mirror failure
mode — the cleanup drops nothing because the namespace no longer resolves — leaves the
orphan `system.buckets.tmp.agg_out.<uuid>` collection behind indefinitely.

## Fix

`OutStage` already tracks `_tempNsUUID` (set in `retrieveTemporaryCollectionUUID`,
asserted in `checkTemporaryCollectionUUIDNotChanged`, and used to gate inserts via
`setCollectionUUID`). The cleanup destructor needs the same gate. The post-rename
namespace inherits the temp collection's UUID through `renameCollection`, so
`_tempNsUUID` is still the correct discriminator at cleanup time.

Concretely:

1. Widen `MongoProcessInterface::dropTempCollection` to accept an optional
   `expectedUUID`. The shard-server and standalone implementations forward this to
   `dropCollection` via `CollectionUUIDMismatch`-aware paths (the kernel already exposes
   UUID-gated drop via `dropCollection(opCtx, nss, expectedUUID)` and the
   `collectionUUID` field on the `drop` command).
2. In `OutStage::doDispose`, pass `_tempNsUUID` for both the `kRenameComplete` drop of
   the renamed buckets namespace and the `kViewCreatedIfNeeded` cleanup of `_tempNs`.
   For the `kTmpCollExists` branch the UUID is also already known.
3. Treat `CollectionUUIDMismatch` and `NamespaceNotFound` as benign in the destructor —
   they mean "someone else cleaned up or replaced our collection, and that's fine."
   `LOGV2_WARNING(7466203, …)` already swallows other `DBException`s; widen the
   message to record the expected UUID for forensics.

## Test plan

The regression test (`jstests/aggregation/sources/out/out_timeseries_cleanup_no_orphan_buckets.js`)
drives the cleanup path under concurrent `$out` activity targeting the same time-series
namespace. Pre-fix, repeated runs against a non-UUID-gated drop will leak
`system.buckets.tmp.agg_out.<uuid>` collections; the test asserts those are absent at the
end. The existing sharded test
(`jstests/noPassthrough/query/out_merge/out_timeseries_cleans_up_bucket_collections.js`)
continues to cover the post-rename failpoint path on multi-shard topologies.

## Risks / out of scope

Bounded to legacy viewful time-series collections (`legacy_timeseries_only_bug` label).
Viewless time-series goes through the regular cleanup branch with a UUID already; UUID
gating there is purely defensive. No on-disk format change, no FCV gate.
