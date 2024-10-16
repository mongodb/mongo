/**
 * Tests that idle buckets are removed when the bucket catalog's memory threshold is reached.
 *
 * @tags: [
 *  requires_replication,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet({setParameter: {timeseriesIdleBucketExpiryMemoryUsageThreshold: 10485760}});
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());

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
    assert.commandWorked(coll.insert([
        {
            [timeFieldName]: ISODate(),
            [metaFieldName]: {[i.toString()]: metaValue},
            [valueFieldName]: 0
        },
        {
            [timeFieldName]: ISODate(),
            [metaFieldName]: {[i.toString()]: metaValue},
            [valueFieldName]: 1
        },
        {
            [timeFieldName]: ISODate(),
            [metaFieldName]: {[i.toString()]: metaValue},
            [valueFieldName]: 3
        }
    ]));
}

// Now go back and insert documents with the same metadata, and verify that we at some point
// insert into a new bucket, indicating the old one was expired.
let foundExpiredBucket = false;
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(coll.insert({
        [timeFieldName]: ISODate(),
        [metaFieldName]: {[i.toString()]: metaValue},
        [valueFieldName]: 3
    }));

    // Check buckets.
    let bucketDocs =
        bucketsColl.find({"control.version": TimeseriesTest.BucketVersion.kCompressedSorted})
            .limit(1)
            .toArray();
    if (bucketDocs.length > 0) {
        foundExpiredBucket = true;
    }
}
assert(foundExpiredBucket, "Did not find an expired bucket");

rst.stopSet();
