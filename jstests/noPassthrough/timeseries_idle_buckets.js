/**
 * Tests that idle buckets are removed when the bucket catalog's memory threshold is reached.
 *
 * @tags: [
 *  requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

const rst = new ReplSetTest({nodes: 1});
rst.startSet({setParameter: {timeseriesIdleBucketExpiryMemoryUsageThreshold: 10485760}});
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());

const isTimeseriesBucketCompressionEnabled = TimeseriesTest.timeseriesBucketCompressionEnabled(db);
assert.commandWorked(db.dropDatabase());

const coll = db.timeseries_idle_buckets;
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

const timeFieldName = 'time';
const metaFieldName = 'meta';
const valueFieldName = 'value';

coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
assert.contains(bucketsColl.getName(), db.getCollectionNames());

// Insert enough documents with large enough metadata so that the bucket catalog memory
// threshold is reached and idle buckets are expired.
const numDocs = 100;
const metaValue = 'a'.repeat(1024 * 1024);
for (let i = 0; i < numDocs; i++) {
    // Insert a couple of measurements in the bucket to make sure compression is triggered if
    // enabled
    assert.commandWorked(coll.insert({
        [timeFieldName]: ISODate(),
        [metaFieldName]: {[i.toString()]: metaValue},
        [valueFieldName]: 0
    }));
    assert.commandWorked(coll.insert({
        [timeFieldName]: ISODate(),
        [metaFieldName]: {[i.toString()]: metaValue},
        [valueFieldName]: 1
    }));
    assert.commandWorked(coll.insert({
        [timeFieldName]: ISODate(),
        [metaFieldName]: {[i.toString()]: metaValue},
        [valueFieldName]: 3
    }));
}

// No go back and insert documents with the same metadata, and verify that we at some point
// insert into a new bucket, indicating the old one was expired.
let foundExpiredBucket = false;
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(coll.insert({
        [timeFieldName]: ISODate(),
        [metaFieldName]: {[i.toString()]: metaValue},
        [valueFieldName]: 3
    }));

    // Check buckets.
    let bucketDocs = bucketsColl.find({meta: {[i.toString()]: metaValue}})
                         .sort({'control.min._id': 1})
                         .toArray();
    if (bucketDocs.length > 1) {
        // If bucket compression is enabled the expired bucket should have been compressed
        assert.eq(isTimeseriesBucketCompressionEnabled ? 2 : 1,
                  bucketDocs[0].control.version,
                  'unexpected control.version in first bucket: ' + tojson(bucketDocs));
        assert.eq(1,
                  bucketDocs[1].control.version,
                  'unexpected control.version in second bucket: ' + tojson(bucketDocs));

        foundExpiredBucket = true;
        break;
    } else {
        // The insert landed in an existing bucket, verify that compression didn't take place yet.
        assert.eq(
            bucketDocs.length,
            1,
            'Invalid number of buckets for metadata ' + (numDocs - 1) + ': ' + tojson(bucketDocs));
        assert.eq(1,
                  bucketDocs[0].control.version,
                  'unexpected control.version in second bucket: ' + tojson(bucketDocs));
    }
}
assert(foundExpiredBucket, "Did not find an expired bucket");

rst.stopSet();
})();
