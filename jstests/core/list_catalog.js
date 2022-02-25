/**
 * Basic tests for the $listCatalog aggregation stage.
 *
 * @tags: [
 *     # This test creates views and time-series collections, and drops these namespaces
 *     # as a pre-processing step. We should not allow the test framework to create these
 *     # namespaces as regular collections.
 *     assumes_no_implicit_collection_creation_after_drop,
 *     # Time-series collection inserts are not supported in multi-document transactions.
 *     does_not_support_transactions,
 *     requires_fcv_60,
 *     # $listCatalog result can be large and may be returned in multiple batches.
 *     requires_getmore,
 * ]
 */
(function() {
'use strict';

const documentSourceListCatalogEnabled =
    assert
        .commandWorked(
            db.getMongo().adminCommand({getParameter: 1, featureFlagDocumentSourceListCatalog: 1}))
        .featureFlagDocumentSourceListCatalog.value;

if (!documentSourceListCatalogEnabled) {
    jsTestLog('Skipping test because the $listCatalog aggregation stage feature flag is disabled.');
    return;
}

const collNamePrefix = 'list_catalog_';

// Simple collection with one secondary index.
const collSimple = db.getCollection(collNamePrefix + 'simple');
collSimple.drop();
assert.commandWorked(collSimple.createIndex({a: 1}));
assert.commandWorked(collSimple.insert({_id: 0, a: 0}));

// Simple view with no pipeline.
const viewSimple = db.getCollection(collNamePrefix + 'simple_view');
viewSimple.drop();
assert.commandWorked(db.createView(viewSimple.getName(), collSimple.getName(), []));

// Time-series collection.
const collTimeseries = db.getCollection(collNamePrefix + 'ts');
collTimeseries.drop();
assert.commandWorked(
    db.createCollection(collTimeseries.getName(), {timeseries: {timeField: 'tt'}}));
assert.commandWorked(collTimeseries.insert({_id: 1, tt: ISODate(), x: 123}));

// Collection with clustered index.
const collClustered = db.getCollection(collNamePrefix + 'clustered');
collClustered.drop();
assert.commandWorked(
    db.createCollection(collClustered.getName(), {clusteredIndex: {key: {_id: 1}, unique: true}}));
assert.commandWorked(collClustered.insert({_id: 2, y: 'abc'}));

const adminDB = db.getSiblingDB('admin');
const result = adminDB.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog('$listCatalog result: ' + tojson(result));

const catalogEntries = Object.assign({}, ...result.map(doc => ({[doc.ns]: doc})));
jsTestLog('Catalog entries keyed by namespace: ' + tojson(catalogEntries));

const entryCollSimple = catalogEntries[collSimple.getFullName()];
assert(entryCollSimple, 'simple collection not found: ' + tojson(result));
assert.eq('collection', entryCollSimple.type, tojson(entryCollSimple));
const hasIdIndexCollSimple = !entryCollSimple.md.options.clusteredIndex;
assert.eq(hasIdIndexCollSimple ? 2 : 1, entryCollSimple.md.indexes.length, tojson(entryCollSimple));

const entryViewSimple = catalogEntries[viewSimple.getFullName()];
assert(entryViewSimple, 'simple view not found: ' + tojson(result));
assert.eq('view', entryViewSimple.type, tojson(entryViewSimple));
assert.eq(collSimple.getName(), entryViewSimple.viewOn, tojson(entryViewSimple));

const entryTimeseries = catalogEntries[collTimeseries.getFullName()];
assert(entryTimeseries, 'time-series collection not found: ' + tojson(result));
assert.eq('timeseries', entryTimeseries.type, tojson(entryTimeseries));
const bucketsCollectionName = 'system.buckets.' + collTimeseries.getName();
assert.eq(bucketsCollectionName, entryTimeseries.viewOn, tojson(entryTimeseries));
const entryBucketsCollection =
    catalogEntries[db.getCollection(bucketsCollectionName).getFullName()];
assert(entryBucketsCollection, 'buckets collection not found: ' + tojson(result));

const entryCollClustered = catalogEntries[collClustered.getFullName()];
assert(entryCollClustered, 'clustered collection not found: ' + tojson(result));
assert.eq('collection', entryCollClustered.type, tojson(entryCollClustered));
assert.sameMembers([], entryCollClustered.md.indexes, tojson(entryCollClustered));
})();
