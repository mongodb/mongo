/**
 * Tests insertion of data to time series buckets after they have had their meta field updated.
 * @tags: [
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

const conn = MongoRunner.runMongod({});
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
const bucketsColl = db.system.buckets[jsTestName()];

// Create a new bucket.
assert.commandWorked(coll.insert({m: 1, t: ISODate("2024-06-06T00:00:00.000Z")}));
assert.eq(coll.find({m: 1}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 1}).toArray().length, 1);

// Update correctly changes meta field for existing bucket.
assert.commandWorked(coll.updateMany({m: 1}, {$set: {m: 2}}));
assert.eq(coll.find({m: 1}).toArray().length, 0);
assert.eq(bucketsColl.find({meta: 1}).toArray().length, 0);
assert.eq(coll.find({m: 2}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 2}).toArray().length, 1);

// Inserting with the old meta finds the bucket has been cleared, inserts new bucket.
assert.commandWorked(coll.insert({m: 1, t: ISODate("2024-06-06T00:00:00.000Z")}));
assert.eq(coll.find({m: 1}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 1}).toArray().length, 1);
assert.eq(coll.find({m: 2}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 2}).toArray().length, 1);

// Create a new bucket.
assert.commandWorked(coll.insert({m: 3, t: ISODate("2024-06-06T00:00:00.000Z")}));
assert.eq(coll.find({m: 3}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 3}).toArray().length, 1);

// Update correctly changes meta field for existing bucket.
assert.commandWorked(coll.updateMany({m: 3}, {$set: {m: 4}}));
assert.eq(coll.find({m: 3}).toArray().length, 0);
assert.eq(bucketsColl.find({meta: 3}).toArray().length, 0);
assert.eq(coll.find({m: 4}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 4}).toArray().length, 1);

// Inserting with the new meta reopens the existing bucket and succeeds.
assert.commandWorked(coll.insert({m: 4, t: ISODate("2024-06-06T00:00:00.000Z")}));
assert.eq(coll.find({m: 3}).toArray().length, 0);
assert.eq(bucketsColl.find({meta: 3}).toArray().length, 0);
assert.eq(coll.find({m: 4}).toArray().length, 2);
assert.eq(bucketsColl.find({meta: 4}).toArray().length, 1);

// Inserting with old meta now accesses a different stripe in the bucket catalog compared to the new
// meta. We should still find an old record of the bucket, marked cleared, and open a new bucket.
let res = assert.commandWorked(coll.insert({m: 3, t: ISODate("2024-06-06T00:00:00.000Z")}));
assert.eq(res.nInserted, 1);
assert.eq(coll.find({m: 3}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 3}).toArray().length, 1);
assert.eq(coll.find({m: 4}).toArray().length, 2);
assert.eq(bucketsColl.find({meta: 4}).toArray().length, 1);

// Note that the above was a special case where the measurements were identical. Now let's do the
// same setup, but check with a different timestamp.
assert.commandWorked(coll.insert({m: 5, t: ISODate("2024-06-06T00:00:00.000Z")}));
assert.eq(coll.find({m: 5}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 5}).toArray().length, 1);
assert.commandWorked(coll.updateMany({m: 5}, {$set: {m: 6}}));
assert.eq(coll.find({m: 5}).toArray().length, 0);
assert.eq(bucketsColl.find({meta: 5}).toArray().length, 0);
assert.eq(coll.find({m: 6}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 6}).toArray().length, 1);
assert.commandWorked(coll.insert({m: 6, t: ISODate("2024-06-06T00:00:00.000Z")}));
assert.eq(coll.find({m: 5}).toArray().length, 0);
assert.eq(bucketsColl.find({meta: 5}).toArray().length, 0);
assert.eq(coll.find({m: 6}).toArray().length, 2);
assert.eq(bucketsColl.find({meta: 6}).toArray().length, 1);

res = assert.commandWorked(coll.insert({m: 5, t: ISODate("2024-06-06T00:00:00.001Z")}));
assert.eq(res.nInserted, 1);
assert.eq(coll.find({m: 5}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 5}).toArray().length, 1);
assert.eq(coll.find({m: 6}).toArray().length, 2);
assert.eq(bucketsColl.find({meta: 6}).toArray().length, 1);

// Now we check what happens with different data in the measurements, in addition to different
// timestamps.
assert.commandWorked(coll.insert({m: 7, t: ISODate("2024-06-06T00:00:00.000Z"), x: 1}));
assert.eq(coll.find({m: 7}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 7}).toArray().length, 1);
assert.commandWorked(coll.updateMany({m: 7}, {$set: {m: 8}}));
assert.eq(coll.find({m: 7}).toArray().length, 0);
assert.eq(bucketsColl.find({meta: 7}).toArray().length, 0);
assert.eq(coll.find({m: 8}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 8}).toArray().length, 1);
assert.commandWorked(coll.insert({m: 8, t: ISODate("2024-06-06T00:00:00.000Z"), x: 2}));
assert.eq(coll.find({m: 7}).toArray().length, 0);
assert.eq(bucketsColl.find({meta: 7}).toArray().length, 0);
assert.eq(coll.find({m: 8}).toArray().length, 2);
assert.eq(bucketsColl.find({meta: 8}).toArray().length, 1);

res = assert.commandWorked(coll.insert({m: 7, t: ISODate("2024-06-06T00:00:00.001Z"), x: 3}));
assert.eq(res.nInserted, 1);
assert.eq(coll.find({m: 7}).toArray().length, 1);
assert.eq(bucketsColl.find({meta: 7}).toArray().length, 1);
assert.eq(coll.find({m: 8}).toArray().length, 2);
assert.eq(bucketsColl.find({meta: 8}).toArray().length, 1);

MongoRunner.stopMongod(conn);
})();