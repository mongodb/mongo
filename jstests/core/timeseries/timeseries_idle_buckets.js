/**
 * Tests that idle buckets are removed when the bucket catalog's memory threshold is reached.
 *
 * @tags: [
 *     assumes_unsharded_collection,         # TODO(SERVER-53816): remove
 *     does_not_support_causal_consistency,  # TODO(SERVER-53819): remove
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
const metaFieldName = 'meta';

assert.commandWorked(testDB.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

// Insert enough documents with large enough metadata so that the bucket catalog memory threshold is
// reached and idle buckets are expired.
const numDocs = 50;
const metaValue = 'a'.repeat(1024 * 1024);
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(
        coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: {[i.toString()]: metaValue}}));
}

// Insert a document with the metadata of a bucket which should have been expired. Thus, a new
// bucket will be created.
assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: {0: metaValue}}));
let bucketDocs = bucketsColl.find({meta: {0: metaValue}}).toArray();
assert.eq(bucketDocs.length, 2, 'Invalid number of buckets for metadata 0: ' + tojson(bucketDocs));

// Insert a document with the metadata of a bucket with should still be open. Thus, the existing
// bucket will be used.
assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: {[numDocs - 1]: metaValue}}));
bucketDocs = bucketsColl.find({meta: {[numDocs - 1]: metaValue}}).toArray();
assert.eq(bucketDocs.length,
          1,
          'Invalid number of buckets for metadata ' + (numDocs - 1) + ': ' + tojson(bucketDocs));
})();