/**
 * Tests that an oversized time-series unordered bucket update triggers the size
 * check in performAtomicTimeseriesWrites().
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const timeField = "t";
const timestamp = ISODate("2025-01-01T00:00:00Z");
const collName = "tscoll";

assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: timeField}}));
const coll = db[collName];

// Phase 1: Insert 5 measurements with large "z" values to build up bucket state.
// Using "z" because "a" < "z", so phase 2's "a" values will update control.min.
const numMeasurements = 5;
const phase1LongStrSize = 200000;
const numFields = 4;
const phase1Val = "z".repeat(phase1LongStrSize);
const phase1Measurement = {
    [timeField]: timestamp
};
for (let i = 0; i < numFields; i++) {
    phase1Measurement["a" + i] = phase1Val;
}

jsTestLog("Phase 1: Inserting " + numMeasurements + " measurements with 'z'*" + phase1LongStrSize +
          " to build up bucket.");
for (let i = 0; i < numMeasurements; i++) {
    assert.commandWorked(coll.insert(phase1Measurement, {ordered: true}));
}

let stats = coll.stats().timeseries;
assert.eq(stats.numBucketInserts, 1);
assert.eq(stats.numBucketUpdates, 4);

// Phase 2: Insert 1 unordered measurement with large "a" values.
// "a" < "z" yields control.min.a[i] updates from a large "z" to a large "a" for each field.
const phase2LongStrSize = 1650000;
const phase2Measurement = {
    [timeField]: timestamp
};
const largeVal = "a".repeat(phase2LongStrSize);
for (let i = 0; i < numFields; i++) {
    phase2Measurement["a" + i] = largeVal;
}

jsTestLog("Phase 2: Inserting measurement with 'a'*" + phase2LongStrSize + " (ordered: false).");

assert.commandFailedWithCode(coll.insert(phase2Measurement, {ordered: false}),
                             ErrorCodes.BSONObjectTooLarge);

// The attempted update failed, so stats should be the same as before.
stats = coll.stats().timeseries;
assert.eq(stats.numBucketInserts, 1);
assert.eq(stats.numBucketUpdates, 4);

MongoRunner.stopMongod(conn);
