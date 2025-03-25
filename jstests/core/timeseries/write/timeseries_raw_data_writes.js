/**
 * Performs basic write operations on a time-series buckets collection through its time-series view
 * namespace using rawData.
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_transactions,
 *   featureFlagRawDataCrudOperations,
 *   requires_non_retryable_writes,
 *   # TODO (SERVER-101293): Remove this tag.
 *   known_query_shape_computation_problem,
 * ]
 */

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

const testBuckets = coll.find().rawData().toArray();
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
    assert.eq(coll.remove({"control.count": 2}, {rawData: true}).nRemoved, 1);
});

// deleteOne()
crudTest(() => {
    assert.eq(coll.deleteOne({"control.count": 2}, {rawData: true}).deletedCount, 1);
});

// deleteMany()
crudTest(() => {
    assert.eq(coll.deleteMany({"control.version": 2}, {rawData: true}).deletedCount, 2);
});

// findAndModify()
crudTest(() => {
    const newBucket = coll.findAndModify(
        {query: {"control.count": 2}, update: {$set: {meta: "3"}}, new: true, rawData: true});
    assert.eq(newBucket.meta, "3");
});

// findOneAndDelete()
crudTest(() => {
    const deletedBucket = coll.findOneAndDelete({"control.count": 2}, {rawData: true});
    assert.eq(deletedBucket.control.count, 2);
    assert.eq(coll.count({"control.count": 2}, {rawData: true}), 0);
});

// findOneAndReplace()
crudTest(() => {
    const updatedBucket =
        coll.findOne({"control.count": 2}, null, null, null, null, true /* rawData */);
    updatedBucket.meta = "3";
    const replacedBucket =
        coll.findOneAndReplace({"control.count": 2}, updatedBucket, {rawData: true});
    assert.eq(replacedBucket.meta, "1");
    assert.eq(coll.findOne({"control.count": 2}, null, null, null, null, true /* rawData */).meta,
              "3");
});

// findOneAndUpdate()
crudTest(() => {
    const updatedBucket =
        coll.findOneAndUpdate({"control.count": 2}, {$set: {meta: "3"}}, {rawData: true});
    assert.eq(updatedBucket.meta, "1");
    assert.eq(coll.findOne({"control.count": 2}, null, null, null, null, true /* rawData */).meta,
              "3");
});

// insert()
crudTest(() => {
    coll.insert(insertBucket, {rawData: true});
    assert.eq(coll.findOne({}, null, null, null, null, true /* rawData */), insertBucket);
}, false);

// insertOne()
crudTest(() => {
    coll.insertOne(insertBucket, {rawData: true});
    assert.eq(coll.findOne({}, null, null, null, null, true /* rawData */), insertBucket);
    assert.eq(coll.find().rawData().length(), 1);
}, false);

// insertMany()
crudTest(() => {
    coll.insertMany(testBuckets, {rawData: true});
    assert.eq(coll.find().rawData().length(), 2);
}, false);

// update()
crudTest(() => {
    assert.eq(coll.update({"control.count": 2}, {$set: {meta: "3"}}, {rawData: true}).nModified, 1);
    assert.eq(coll.find({"control.count": 2}).rawData().length(), 1);
    assert.eq(coll.find({"control.count": 2}).rawData()[0].meta, "3");
});

// updateOne()
crudTest(() => {
    assert.eq(
        coll.updateOne({"control.count": 2}, {$set: {meta: "3"}}, {rawData: true}).modifiedCount,
        1);
    assert.eq(coll.find({"control.count": 2}).rawData().length(), 1);
    assert.eq(coll.find({"control.count": 2}).rawData()[0].meta, "3");
});

// updateMany()
crudTest(() => {
    assert.eq(
        coll.updateMany({"control.version": 2}, {$set: {meta: "3"}}, {rawData: true}).modifiedCount,
        2);
    assert.eq(coll.find({"meta": "3"}).rawData().length(), 2);
});

// replaceOne()
// Remove the id field from the bucket for this test since it is an immutable field.
delete replaceOneBucket["_id"];
crudTest(() => {
    assert.eq(
        coll.replaceOne({"control.count": 2}, replaceOneBucket, {rawData: true}).modifiedCount, 1);
    assert.eq(coll.find({"meta": "1"}).rawData().length(), 1);
    assert.eq(coll.find({"meta": "1"}).rawData()[0].meta, "1");
    assert.eq(coll.find({"control.max.v": "replacement"}).rawData().length(), 1);
});
