/**
 * Basic tests for the $listCatalog aggregation stage.
 *
 * @tags: [
 *     # Asserts on number of indexes.
 *     assumes_no_implicit_index_creation,
 *     # Time-series collection inserts are not supported in multi-document transactions.
 *     does_not_support_transactions,
 *     requires_fcv_60,
 *     # $listCatalog result can be large and may be returned in multiple batches.
 *     requires_getmore,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fixture_helpers.js');

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

// Simple collection with one secondary index.
const collSimple = testDB.simple;
assert.commandWorked(collSimple.createIndex({a: 1}));
assert.commandWorked(collSimple.insert({_id: 0, a: 0}));

// Simple view.
const viewSimpleName = 'simple_view';
assert.commandWorked(testDB.createView(viewSimpleName, collSimple.getName(), [{$project: {a: 0}}]));

// Time-series collection.
assert.commandWorked(testDB.createCollection('ts', {timeseries: {timeField: 'tt'}}));
const collTimeseries = testDB.ts;
assert.commandWorked(collTimeseries.insert({_id: 1, tt: ISODate(), x: 123}));

// Collection with clustered index.
assert.commandWorked(
    testDB.createCollection('clustered', {clusteredIndex: {key: {_id: 1}, unique: true}}));
const collClustered = testDB.clustered;
assert.commandWorked(collClustered.insert({_id: 2, y: 'abc'}));

const numIndexes = function(coll, entry, numSecondaryIndexes) {
    let numIndexes = numSecondaryIndexes + 1;
    if (entry.md.options.clusteredIndex) {
        --numIndexes;
    }
    if (FixtureHelpers.isSharded(coll)) {
        ++numIndexes;
    }
    return numIndexes;
};

const checkEntries = function(collName, entries, type, {numSecondaryIndexes, viewOn}) {
    const ns = testDB.getName() + '.' + collName;
    assert(entries.some((entry) => entry.ns === ns));
    for (const entry of entries) {
        if (entry.ns !== ns) {
            continue;
        }

        assert.eq(entry.db, testDB.getName());
        assert.eq(entry.name, collName);
        assert.eq(entry.type, type);
        if (FixtureHelpers.isMongos(testDB)) {
            assert(entry.shard);
        }
        if (type === 'collection') {
            assert.eq(entry.md.indexes.length,
                      numIndexes(testDB[collName], entry, numSecondaryIndexes));
        }
        if (type === 'view' || type === 'timeseries') {
            assert.eq(entry.viewOn, viewOn);
        }
    }
};

let result = collSimple.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog(collSimple.getFullName() + ' $listCatalog: ' + tojson(result));
checkEntries(collSimple.getName(), result, 'collection', {numSecondaryIndexes: 1});

result = collClustered.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog(collClustered.getFullName() + ' $listCatalog: ' + tojson(result));
checkEntries(collClustered.getName(), result, 'collection', {numSecondaryIndexes: 0});

assert.commandFailedWithCode(
    testDB.runCommand({aggregate: viewSimpleName, pipeline: [{$listCatalog: {}}], cursor: {}}),
    40602);

assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: collTimeseries.getName(), pipeline: [{$listCatalog: {}}], cursor: {}}),
    40602);

assert.commandFailedWithCode(
    testDB.runCommand({aggregate: 1, pipeline: [{$listCatalog: {}}], cursor: {}}),
    ErrorCodes.InvalidNamespace);

const adminDB = testDB.getSiblingDB('admin');
result = adminDB.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog('Collectionless $listCatalog: ' + tojson(result));

checkEntries(collSimple.getName(), result, 'collection', {numSecondaryIndexes: 1});
checkEntries(collClustered.getName(), result, 'collection', {numSecondaryIndexes: 0});
checkEntries(viewSimpleName, result, 'view', {viewOn: collSimple.getName()});
checkEntries(collTimeseries.getName(), result, 'timeseries', {
    viewOn: 'system.buckets.' + collTimeseries.getName()
});
})();
