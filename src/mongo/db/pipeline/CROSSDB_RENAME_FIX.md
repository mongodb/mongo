# SERVER-107688 — cross-DB rename leaks staging events into change streams

## Symptom

A database-scoped or cluster-scoped change-stream consumer watching the
destination database of a cross-database `renameCollection` observes the
internal data-cloning traffic:

- `create` on `<dstDb>.tmp<nonce>.renameCollection`
- `createIndexes` for each source-collection secondary index, re-applied
  to the staging namespace
- one `insert` per cloned document into the staging namespace

The pre-existing test `jstests/change_streams/ddl_rename_cross_db.js`
already asserts the leak as the observed status quo (see lines 170-179).
External consumers — mongosync, Kafka source connectors, application-level
CDC — try to resume against a namespace that has been atomically renamed
away, and fail.

## Fix location (not modified here)

The change-stream parser already drops `fromMigrate:true` oplog entries.
See `change_stream_filter_helpers.cpp:buildNotFromMigrateFilter` — it
adds a `{fromMigrate: {$ne: true}}` filter, with a narrow `showSystemEvents`
exception for `o.create` / `o.createIndexes`. The parser does not need
new logic. The leak is upstream: the rename plumbing fails to stamp
`fromMigrate=true` on the staging-collection oplog entries during the
data-cloning phase of a cross-DB rename.

The fix lives in the catalog layer (sparse-checkout excluded here):
`src/mongo/db/catalog/rename_collection.cpp`, in the helper that
implements the cross-DB code path (historically
`renameBetweenDBs` / `renameCollectionAcrossDatabases`). That helper
opens an `AutoGetCollection` on the staging namespace and replays the
source documents with `insertDocuments` / `Helpers::insert`.

## Diff sketch (illustrative)

```diff
--- a/src/mongo/db/catalog/rename_collection.cpp
+++ b/src/mongo/db/catalog/rename_collection.cpp
@@ Status renameBetweenDBs(OperationContext* opCtx, ...)
-    // Create the staging collection on the target DB.
-    CollectionOptions stagingOpts = sourceOpts;
-    Status createStatus = db->createCollection(opCtx, stagingNss, stagingOpts);
+    // Create the staging collection on the target DB. Mark the create,
+    // index builds, and per-document inserts that follow as fromMigrate:true
+    // so that change-stream consumers do not observe the data-cloning phase
+    // (SERVER-107688). The terminal atomic rename remains user-visible.
+    CollectionOptions stagingOpts = sourceOpts;
+    stagingOpts.fromMigrate = true;     // tag the create oplog entry
+    Status createStatus = db->createCollection(opCtx, stagingNss, stagingOpts);
@@ for each source index ...
-    indexBuildsCoordinator->createIndexes(opCtx, stagingUUID, specs,
-                                          IndexBuildsManager::IndexConstraints::kEnforce);
+    indexBuildsCoordinator->createIndexes(opCtx, stagingUUID, specs,
+                                          IndexBuildsManager::IndexConstraints::kEnforce,
+                                          /*fromMigrate=*/true);
@@ document-clone loop ...
-    Status insertStatus = collection_internal::insertDocuments(
-        opCtx, *stagingCollection, batch.begin(), batch.end(), nullptr);
+    Status insertStatus = collection_internal::insertDocuments(
+        opCtx, *stagingCollection, batch.begin(), batch.end(), nullptr,
+        /*fromMigrate=*/true);
```

`collection_internal::insertDocuments` already forwards a `fromMigrate`
parameter through `OpObserver::onInserts` → `MutableOplogEntry::fromMigrate`,
which is what `change_stream_filter_helpers.cpp` filters on.

## Out of scope

- The final atomic rename of `tmp<nonce>.renameCollection` →
  `<dstDb>.<dstColl>` must remain user-visible. It is a single oplog
  entry, not part of the data-cloning phase, and is emitted without
  `fromMigrate` today. The fix preserves that.
- Source-DB events (`insert`, `createIndexes`, `drop` of the original
  collection) are untouched; they remain visible on source-scoped streams.
- The existing test `ddl_rename_cross_db.js` will need its assertions on
  staging-namespace events (lines 170-179) inverted to `assertNoChanges`
  semantics as part of landing the catalog fix.

## Verification

`jstests/change_streams/cross_db_rename_no_staging_events.js` exercises
both database-scoped and cluster-scoped (`allChangesForCluster:true`)
watchers, with and without `dropTarget:true`. Pre-fix it fails on the
first staging-namespace event; post-fix it passes because the staging
oplog entries carry `fromMigrate:true` and the parser's existing
`fromMigrate:{$ne:true}` filter drops them.
