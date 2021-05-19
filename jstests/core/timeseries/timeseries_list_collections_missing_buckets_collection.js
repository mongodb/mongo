/**
 * Tests that listCollections shows the time-series view, but not the buckets collection, if the
 * backing time-series buckets collection is missing.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_transactions,
 *   requires_getmore,
 * ]
 */
(function() {
'use strict';

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const timeFieldName = 'time';
const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert(bucketsColl.drop());

let collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.eq(collections.length, 2);
assert(collections.find(entry => entry.name === 'system.views'));
assert.docEq(collections.find(entry => entry.name === coll.getName()),
             {name: coll.getName(), type: 'timeseries', options: {}, info: {readOnly: false}});

collections =
    assert.commandWorked(testDB.runCommand({listCollections: 1, filter: {name: coll.getName()}}))
        .cursor.firstBatch;
assert.eq(collections,
          [{name: coll.getName(), type: 'timeseries', options: {}, info: {readOnly: false}}]);
})();
