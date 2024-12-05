/**
 * Tests measurements from time-series updates have their metadata fields normalized correctly.
 *
 * @tags: [
 *   requires_multi_updates,
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesUpdatesSupport,
 *   requires_getmore,
 * ]
 */

const collName = "test";
const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

function verifyMetaFields(coll) {
    const metaFields = coll.find({}, {time: 0, _id: 0}).toArray();
    for (const metaField of metaFields) {
        assert.eq(0, bsonWoCompare(metaField, {tag: {a: 1, b: {c: 1, d: 1}}}), tojson(metaField));
    }
}

// Insert the initial measurements.
const coll = testDB.getCollection(collName);
assert.commandWorked(testDB.createCollection(coll.getName(), {
    timeseries: {timeField: "time", metaField: "tag"},
}));
for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({time: new Date(), tag: {a: 1, b: {c: 1, d: 1}}, _id: i}));
}

assert.commandWorked(testDB.runCommand({
    update: coll.getName(),
    updates: [{q: {}, u: {$set: {tag: {b: {d: 1, c: 1}, a: 1}}}, multi: true}]
}));
verifyMetaFields(coll);

assert.commandWorked(testDB.runCommand({
    update: coll.getName(),
    updates: [{q: {}, u: {$set: {tag: {b: {d: 1, c: 1}, a: 1}}}, multi: false}]
}));
verifyMetaFields(coll);

// Skip in sharded passthrough suites since query on time-series time field cannot target the
// command to one single shard.
if (!db.getMongo().isMongos() && !TestData.testingReplicaSetEndpoint) {
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [{
            q: {time: ISODate("2023-01-01T12:00:02.000Z")},
            u: {$set: {tag: {b: {d: 1, c: 1}, a: 1}}},
            multi: true,
            upsert: true
        }]
    }));
    verifyMetaFields(coll);
}

assert.commandWorked(testDB.runCommand({
    update: coll.getName(),
    updates: [{
        q: {time: new Date()},
        u: {$set: {tag: {b: {d: 1, c: 1}, a: 1}}},
        multi: false,
        upsert: true
    }]
}));
verifyMetaFields(coll);
