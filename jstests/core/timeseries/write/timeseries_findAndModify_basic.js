/**
 * Tests basic findAndModify commands against time-series collection.
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # Retryable findAndModify is not supported on time-series collections
 *   does_not_support_retryable_writes,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

const timeFieldName = "time";
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const numDocs = 10;
const time = ISODate("2025-08-20T18:00:00Z");
const newTime = ISODate("2025-08-20T18:10:00Z");

let toInsert = [];
for (let i = 0; i < numDocs; i++) {
    toInsert.push({_id: i, [timeFieldName]: time});
}
toInsert.forEach((doc) => coll.insertOne(doc));

// Test findAndModify update queries.
{
    const doc = coll.findAndModify({query: {_id: {$gt: 0}}, update: {$set: {t2: newTime}}});
    assert.lt(0, doc._id);
    assert.eq(1, coll.find({t2: {$exists: true}}).toArray().length);
}

// Test findAndModify rawData update queries.
{
    const doc = coll.findAndModify({query: {}, update: {$set: {"control.max._id": 100}}, rawData: true});
    assert.eq(1, coll.find({"control.max._id": 100}).rawData().toArray().length);
    assert.eq(numDocs - 1, coll.find({_id: {$gt: 0}}).toArray().length);
}

// Test findAndModify delete queries.
{
    const doc = coll.findAndModify({query: {t2: {$exists: true}}, remove: true});
    assert.eq(true, "t2" in doc);
    assert.eq(0, coll.find({t2: {$exists: true}}).toArray().length);
    // Restore the collection back to normal.
    coll.insertOne(doc);
}

// Test findAndModify rawData delete queries.
{
    const doc = coll.findAndModify({query: {}, remove: true});
    assert.eq(0, coll.find({_id: doc._id}).rawData().toArray().length);
    assert.gt(numDocs, coll.find({}).toArray().length);
}
