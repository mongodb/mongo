# SERVER-95267 fix design — drop of tracked unsplittable buckets collection

## Bug

When `system.buckets.X` is registered in `config.collections` with
`unsplittable: true`, dropping the collection (via either the logical namespace
`X` or the buckets namespace `system.buckets.X`) leaves a dangling
`config.collections` row referencing the deleted buckets namespace.

The leak appears when the **main** namespace `X` is one of:

1. **Empty** — no view, no collection at `X`.
2. **Shadowed by an unrelated view** at `X`.
3. **Shadowed by a normal (non-timeseries) collection** at `X`.

In all three cases the drop is downgraded to a local-only drop on the primary
shard and never reaches `DropCollectionCoordinator`, so the global-catalog
removal never runs.

## Where the bug surfaces today

### Router (`drop` on mongos)

`src/mongo/db/global_catalog/ddl/cluster_drop_collection_cmd.cpp` routes the
user-supplied `nss` to the primary shard as `ShardsvrDropCollection` without
performing any view-resolution to map a buckets-only tracked entry back to its
logical namespace.

### Shard server entry point

`src/mongo/db/global_catalog/ddl/shardsvr_drop_collection_command.cpp` blocks
direct `system.buckets.*` drops only when both `getTimeseriesFields()` is
set on the catalog entry **and** `coll.getUnsplittable()` is false — i.e. the
sharded-timeseries case. For an unsplittable buckets collection the guard
passes through and the coordinator is created on whatever namespace the user
typed.

### Coordinator

`src/mongo/db/global_catalog/ddl/drop_collection_coordinator.cpp`
(`_checkPreconditionsAndSaveArgumentsOnDoc`) calls
`catalogClient()->getCollection(opCtx, nss())` on the user-supplied namespace:

- When `nss = X` (logical) and only `system.buckets.X` is tracked, this throws
  `NamespaceNotFound` → `_doc.setCollInfo(boost::none)` → coordinator treats the
  drop as **untracked**. The participant proceeds, the local view (or normal
  coll, or no-op) is dropped, but `config.collections` for `system.buckets.X`
  is never cleaned.
- When `nss = system.buckets.X`, the shard catalog helper
  `timeseries::acquireCollectionOrViewPlusTimeseriesView` resolves to the
  buckets coll; the local drop in
  `src/mongo/db/shard_role/shard_catalog/drop_collection.cpp:379` runs the
  buckets-collection branch, but the coordinator's `setCollInfo` lookup was on
  `system.buckets.X` and so it deletes data on the wrong path while the
  primary-shard local drop already ran outside the coordinator on the
  participant side.

## Fix site

`DropCollectionCoordinator::_checkPreconditionsAndSaveArgumentsOnDoc` in
`src/mongo/db/global_catalog/ddl/drop_collection_coordinator.cpp` (around
lines 228-263) is the canonical place to canonicalize the namespace before
caching `CollInfo`.

Make the function:

1. If `nss().isTimeseriesBucketsCollection()`, do not transform; otherwise
   compute the candidate buckets namespace
   `nss().makeTimeseriesBucketsNamespace()`.
2. Query `catalogClient()->getCollection(opCtx, nss())` first. If it returns
   an entry with `getTimeseriesFields()` set AND `getUnsplittable() == true`,
   keep it.
3. Otherwise, if the candidate buckets namespace exists in
   `config.collections` as `unsplittable: true` with `timeseriesFields`,
   adopt **that** entry as `_doc.setCollInfo(...)` AND rewrite the
   coordinator's working namespace to the buckets namespace via the existing
   `kRecoverable` doc-update path before transitioning past `kCheckPreconditions`.
4. Symmetrically, if the user passed the buckets namespace and only the
   buckets entry is tracked (the existing happy path for sharded timeseries),
   leave behavior unchanged.

Once `_doc.getCollInfo()` is populated correctly, the rest of the coordinator
(`_freezeMigrations`, `_enterCriticalSection`, `_commitDropCollection`) already
removes the `config.collections` row, removes zones, logs placement history,
and dispatches the local participant drop — closing the inconsistency window.

### Router-side hardening (optional but cheap)

`cluster_drop_collection_cmd.cpp` can be taught to consult the routing cache
for the buckets namespace when the logical namespace has no routing entry; that
shortens the round-trip but the authoritative correctness fix is on the
coordinator side because the router cannot hold the DDL lock.

## Test

`jstests/sharding/timeseries/drop_tracked_unsplittable_buckets_metadata_consistency.js`
exercises all three scenarios (empty / unrelated view / normal coll at the
main namespace), drops via both the logical and buckets namespaces, and asserts:

- the `config.collections` row for `system.buckets.X` is removed, and
- `checkMetadataConsistency` at admin level returns zero inconsistencies.

The test fails on current HEAD and should pass once
`_checkPreconditionsAndSaveArgumentsOnDoc` canonicalizes the namespace.

## Multi-version considerations

- `featureFlagTrackUnshardedCollectionsUponCreation` gates the scenario where
  a buckets collection becomes tracked-and-unsplittable on creation; gate the
  jstest behind that flag plus `requires_fcv_80`.
- The dangling row already exists in clusters upgraded from earlier versions;
  ship a one-shot cleanup as a `setFCV` upgrade step (or a separate ticket) to
  prune `config.collections` rows whose buckets collection no longer exists on
  any shard, mirroring the cleanup pattern in
  `removeCollAndChunksMetadataFromConfig`.
