/**
 * Performs basic CRUD operations on a time-series buckets collection.
 * @tags: [
 *   does_not_support_transactions,
 *   requires_fcv_81,
 *   requires_non_retryable_writes,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// Set up some testing buckets to work with
const timeField = "t";
const metaField = "m";
const t = new Date("2002-05-29T00:00:00Z");

const coll = db[jsTestName()];
const bucketsColl = db["system.buckets." + coll.getName()];

db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
assert.commandWorked(coll.insertMany([
    {[timeField]: t, [metaField]: "1", v: "replacement"},
    {[timeField]: t, [metaField]: "2", v: "baz"},
    {[timeField]: t, [metaField]: "2", v: "qux"},
]));

const testBuckets = bucketsColl.find().toArray();
const insertBucket = testBuckets.filter(bucket => bucket.meta == "2")[0];
const replaceOneBucket = testBuckets.filter(bucket => bucket.meta == "1")[0];

coll.drop();

function crudTest(fn, addStartingMeasurements = true) {
    db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
    if (addStartingMeasurements) {
        assert.commandWorked(coll.insertMany([
            {[timeField]: t, [metaField]: "1", v: "foo"},
            {[timeField]: t, [metaField]: "1", v: "bar"},
            {[timeField]: t, [metaField]: "2", v: "baz"},
        ]));
    }
    fn();
    assert(coll.drop());
}

// aggregate()
crudTest(() => {
    const buckets = bucketsColl.find().toArray();
    assert.eq(buckets.length, 2);
    const correctBucket = buckets.filter(bucket => bucket.control.count == 2)[0];
    assert(correctBucket);
    const agg = bucketsColl.aggregate([
        {$match: {"control.count": 2}},
    ]);
    assert.eq(agg.toArray().length, 1);
});

// bulkWrite()
crudTest(() => {
    const res = bucketsColl.bulkWrite([
        {insertOne: {document: insertBucket}},
        {deleteOne: {filter: {"_id": insertBucket["_id"]}}},
    ]);
    assert(res.acknowledged);
    assert.eq(res.insertedCount, 1);
    assert.eq(res.deletedCount, 1);
}, false);

// count()
crudTest(() => {
    // TODO (SERVER-99409): Run this test case when the collection is sharded or unsplittable.
    if (FixtureHelpers.isSharded(bucketsColl) || FixtureHelpers.isUnsplittable(bucketsColl)) {
        return;
    }
    assert.eq(coll.count({"control.count": 2}, {rawData: true}), 1);
});

// remove()
crudTest(() => {
    assert.eq(bucketsColl.remove({"control.count": 2}).nRemoved, 1);
});

// deleteOne()
crudTest(() => {
    assert.eq(bucketsColl.deleteOne({"control.count": 2}).deletedCount, 1);
});

// deleteMany()
crudTest(() => {
    assert.eq(bucketsColl.deleteMany({"control.version": 2}).deletedCount, 2);
});

// distinct()
crudTest(() => {
    assert.eq(bucketsColl.distinct("control.count").sort(), [1, 2]);
});

// find()
crudTest(() => {
    assert.eq(bucketsColl.find().length(), 2);
    assert.eq(bucketsColl.find({"control.count": 2}).length(), 1);
});

// findOne()
crudTest(() => {
    const retrievedBucket = bucketsColl.findOne({"control.count": 2});
    assert.eq(retrievedBucket.control.count, 2);
    assert.eq(retrievedBucket.meta, "1");
});

// findAndModify()
crudTest(() => {
    const newBucket = bucketsColl.findAndModify({
        query: {"control.count": 2},
        update: {$set: {meta: "3"}},
        new: true,
    });
    assert.eq(newBucket.meta, "3");
});

// findOneAndDelete()
crudTest(() => {
    const deletedBucket = bucketsColl.findOneAndDelete({"control.count": 2});
    assert.eq(deletedBucket.control.count, 2);
    assert.eq(bucketsColl.count({"control.count": 2}), 0);
});

// findOneAndReplace()
crudTest(() => {
    const updatedBucket = bucketsColl.findOne({"control.count": 2});
    updatedBucket.meta = "3";
    const replacedBucket = bucketsColl.findOneAndReplace({"control.count": 2}, updatedBucket);
    assert.eq(replacedBucket.meta, "1");
    assert.eq(bucketsColl.findOne({"control.count": 2}).meta, "3");
});

// findOneAndUpdate()
crudTest(() => {
    const updatedBucket = bucketsColl.findOneAndUpdate({"control.count": 2}, {$set: {meta: "3"}});
    assert.eq(updatedBucket.meta, "1");
    assert.eq(bucketsColl.findOne({"control.count": 2}).meta, "3");
});

// insert()
crudTest(() => {
    bucketsColl.insert(insertBucket);
    assert.eq(bucketsColl.findOne(), insertBucket);
}, false);

// insertOne()
crudTest(() => {
    bucketsColl.insertOne(insertBucket);
    assert.eq(bucketsColl.findOne(), insertBucket);
    assert.eq(bucketsColl.find().length(), 1);
}, false);

// insertMany()
crudTest(() => {
    bucketsColl.insertMany(testBuckets);
    assert.eq(bucketsColl.find().length(), 2);
}, false);

// update()
crudTest(() => {
    assert.eq(bucketsColl.update({"control.count": 2}, {$set: {meta: "3"}}).nModified, 1);
    assert.eq(bucketsColl.find({"control.count": 2}).length(), 1);
    assert.eq(bucketsColl.find({"control.count": 2})[0].meta, "3");
});

// updateOne()
crudTest(() => {
    assert.eq(bucketsColl.updateOne({"control.count": 2}, {$set: {meta: "3"}}).modifiedCount, 1);
    assert.eq(bucketsColl.find({"control.count": 2}).length(), 1);
    assert.eq(bucketsColl.find({"control.count": 2})[0].meta, "3");
});

// updateMany()
crudTest(() => {
    assert.eq(bucketsColl.updateMany({"control.version": 2}, {$set: {meta: "3"}}).modifiedCount, 2);
    assert.eq(bucketsColl.find({"meta": "3"}).length(), 2);
});

// replaceOne()
// Remove the id field from the bucket for this test since it is an immutable field.
delete replaceOneBucket["_id"];
crudTest(() => {
    assert.eq(bucketsColl.replaceOne({"control.count": 2}, replaceOneBucket).modifiedCount, 1);
    assert.eq(bucketsColl.find({"meta": "1"}).length(), 1);
    assert.eq(bucketsColl.find({"meta": "1"})[0].meta, "1");
    assert.eq(bucketsColl.find({"control.max.v": "replacement"}).length(), 1);
});
