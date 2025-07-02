/**
 * Performs basic write operations on the buckets of a time-series collection using rawData.
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_transactions,
 *   requires_non_retryable_writes,
 *   does_not_support_viewless_timeseries_yet,
 * ]
 */

import {
    getTimeseriesCollForRawOps,
    kRawOperationSpec
} from "jstests/core/libs/raw_operation_utils.js";

// Set up some testing buckets to work with
const timeField = "t";
const metaField = "m";
const t = new Date("2002-05-29T00:00:00Z");

const coll = db[jsTestName()];

assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));
assert.commandWorked(coll.insertMany([
    {[timeField]: t, [metaField]: "1", v: "replacement"},
    {[timeField]: t, [metaField]: "2", v: "baz"},
    {[timeField]: t, [metaField]: "2", v: "qux"},
]));

const testBuckets = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
const insertBucket = testBuckets.filter(bucket => bucket.meta == "2")[0];
const replaceOneBucket = testBuckets.filter(bucket => bucket.meta == "1")[0];

assert(coll.drop());

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

// remove()
crudTest(() => {
    assert.eq(
        getTimeseriesCollForRawOps(coll).remove({"control.count": 2}, kRawOperationSpec).nRemoved,
        1);
});

// deleteOne()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll)
                  .deleteOne({"control.count": 2}, kRawOperationSpec)
                  .deletedCount,
              1);
});

// deleteMany()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll)
                  .deleteMany({"control.version": 2}, kRawOperationSpec)
                  .deletedCount,
              2);
});

// findAndModify()
crudTest(() => {
    const newBucket = getTimeseriesCollForRawOps(coll).findAndModify({
        query: {"control.count": 2},
        update: {$set: {meta: "3"}},
        new: true,
        ...kRawOperationSpec,
    });
    assert.eq(newBucket.meta, "3");
});

// findOneAndDelete()
crudTest(() => {
    const deletedBucket =
        getTimeseriesCollForRawOps(coll).findOneAndDelete({"control.count": 2}, kRawOperationSpec);
    assert.eq(deletedBucket.control.count, 2);
    assert.eq(getTimeseriesCollForRawOps(coll).count({"control.count": 2}, kRawOperationSpec), 0);
});

// findOneAndReplace()
crudTest(() => {
    const updatedBucket = getTimeseriesCollForRawOps(coll).findOne(
        {"control.count": 2}, null, null, null, null, true /* rawData */);
    updatedBucket.meta = "3";
    const replacedBucket = getTimeseriesCollForRawOps(coll).findOneAndReplace(
        {"control.count": 2}, updatedBucket, kRawOperationSpec);
    assert.eq(replacedBucket.meta, "1");
    assert.eq(getTimeseriesCollForRawOps(coll)
                  .findOne({"control.count": 2}, null, null, null, null, true /* rawData */)
                  .meta,
              "3");
});

// findOneAndUpdate()
crudTest(() => {
    const updatedBucket = getTimeseriesCollForRawOps(coll).findOneAndUpdate(
        {"control.count": 2}, {$set: {meta: "3"}}, kRawOperationSpec);
    assert.eq(updatedBucket.meta, "1");
    assert.eq(getTimeseriesCollForRawOps(coll)
                  .findOne({"control.count": 2}, null, null, null, null, true /* rawData */)
                  .meta,
              "3");
});

// insert()
crudTest(() => {
    getTimeseriesCollForRawOps(coll).insert(insertBucket, kRawOperationSpec);
    assert.eq(
        getTimeseriesCollForRawOps(coll).findOne({}, null, null, null, null, true /* rawData */),
        insertBucket);
}, false);

// insertOne()
crudTest(() => {
    getTimeseriesCollForRawOps(coll).insertOne(insertBucket, kRawOperationSpec);
    assert.eq(
        getTimeseriesCollForRawOps(coll).findOne({}, null, null, null, null, true /* rawData */),
        insertBucket);
    assert.eq(getTimeseriesCollForRawOps(coll).find().rawData().length(), 1);
}, false);

// insertMany()
crudTest(() => {
    getTimeseriesCollForRawOps(coll).insertMany(testBuckets, kRawOperationSpec);
    assert.eq(getTimeseriesCollForRawOps(coll).find().rawData().length(), 2);
}, false);

// update()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll)
                  .update({"control.count": 2}, {$set: {meta: "3"}}, kRawOperationSpec)
                  .nModified,
              1);
    assert.eq(getTimeseriesCollForRawOps(coll).find({"control.count": 2}).rawData().length(), 1);
    assert.eq(getTimeseriesCollForRawOps(coll).find({"control.count": 2}).rawData()[0].meta, "3");
});

// updateOne()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll)
                  .updateOne({"control.count": 2}, {$set: {meta: "3"}}, kRawOperationSpec)
                  .modifiedCount,
              1);
    assert.eq(getTimeseriesCollForRawOps(coll).find({"control.count": 2}).rawData().length(), 1);
    assert.eq(getTimeseriesCollForRawOps(coll).find({"control.count": 2}).rawData()[0].meta, "3");
});

// updateMany()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll)
                  .updateMany({"control.version": 2}, {$set: {meta: "3"}}, kRawOperationSpec)
                  .modifiedCount,
              2);
    assert.eq(getTimeseriesCollForRawOps(coll).find({"meta": "3"}).rawData().length(), 2);
});

// replaceOne()
// Remove the id field from the bucket for this test since it is an immutable field.
delete replaceOneBucket["_id"];
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll)
                  .replaceOne({"control.count": 2}, replaceOneBucket, kRawOperationSpec)
                  .modifiedCount,
              1);
    assert.eq(getTimeseriesCollForRawOps(coll).find({"meta": "1"}).rawData().length(), 1);
    assert.eq(getTimeseriesCollForRawOps(coll).find({"meta": "1"}).rawData()[0].meta, "1");
    assert.eq(
        getTimeseriesCollForRawOps(coll).find({"control.max.v": "replacement"}).rawData().length(),
        1);
});
