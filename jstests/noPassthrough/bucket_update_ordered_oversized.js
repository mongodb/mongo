/**
 * Tests that an oversized time-series ordered bucket update triggers the size
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
// "a" < "z", so phase 2's "a" values will force control.min updates (large replacements).
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

// Calibrate phase2LongStrSize dynamically from the actual bucket document size.
// This ensures correctness regardless of BSON encoding overheads.
const bsonObjMaxUserSize = 16 * 1024 * 1024;                    // 16,777,216
const bsonObjMaxInternalSize = bsonObjMaxUserSize + 16 * 1024;  // 16,793,600
const targetResultSize = bsonObjMaxUserSize + 8 * 1024;         // 16,785,408, middle of window

const bucket = db.getCollection("system.buckets." + collName).find().next();
const actualBucketSize = Object.bsonsize(bucket);
jsTestLog("Actual bucket size after phase 1: " + actualBucketSize + " bytes");

// Derive the net byte change when phase 2 adds 1 measurement with J bytes per field.
// Since "a" < "z", each field's control.min is replaced from "z"*200000 to "a"*J.
//
//   [data growth] New entry at array index "5" per field:
//     type(1) + key("5\0")(2) + string_length(4) + value(J) + null(1) = J+8 bytes/field
//     4 fields => 4*(J+8) = 4J+32
//
//   [control.min replacement] Per field, min changes from "z"*200000 to "a"*J:
//     old value bytes: string_length(4) + 200000 + null(1) = 200005
//     new value bytes: string_length(4) + J      + null(1) = J+5
//     net per field:   (J+5) - 200005             = J-200000
//     4 fields => 4*(J-200000) = 4J-800000
//
//   total net change = (4J+32) + (4J-800000) = 8J - 799968
//
//          799968 = 4*200000 - 4*8
//                 = numFields*phase1LongStrSize - numFields*8
//
// targetResultSize = actualBucketSize + 8*J - 799968
//               J  = (targetResultSize - actualBucketSize + 799968) / 8
const phase2LongStrSize = Math.floor((targetResultSize - actualBucketSize + 799968) / 8);
const expectedResultSize = actualBucketSize + 8 * phase2LongStrSize - 799968;

jsTestLog("Calibrated phase2LongStrSize: " + phase2LongStrSize);
jsTestLog("Expected result bucket size: " + expectedResultSize + " bytes");
jsTestLog("BSONObjMaxUserSize: " + bsonObjMaxUserSize +
          ", BSONObjMaxInternalSize: " + bsonObjMaxInternalSize);

assert.gt(phase2LongStrSize, 0, "phase2LongStrSize must be positive");
assert.gt(expectedResultSize,
          bsonObjMaxUserSize,
          "Expected result must exceed BSONObjMaxUserSize to trigger logid 10856505");
assert.lte(expectedResultSize,
           bsonObjMaxInternalSize,
           "Expected result must not exceed BSONObjMaxInternalSize (or else applyDiff will throw)");

// Phase 2: Insert 1 ordered measurement with large "a" values.
// "a" < "z" forces control.min.a[i] updates for each field, growing the bucket over 16MB.
const phase2Measurement = {
    [timeField]: timestamp
};
const largeVal = "a".repeat(phase2LongStrSize);
for (let i = 0; i < numFields; i++) {
    phase2Measurement["a" + i] = largeVal;
}

jsTestLog("Phase 2: Inserting ordered measurement with 'a'*" + phase2LongStrSize + ".");

// The update and re-attempted insert both exceed 16MB and are therefore rejected.
assert.commandFailedWithCode(coll.insert(phase2Measurement, {ordered: true}),
                             ErrorCodes.BSONObjectTooLarge);

// The attempted update failed both ordered and unordered, so stats should be
// the same as before.
stats = coll.stats().timeseries;
assert.eq(stats.numBucketInserts, 1);
assert.eq(stats.numBucketUpdates, 4);

MongoRunner.stopMongod(conn);
