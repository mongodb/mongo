/**
 * Tests that transactions are able to write to the time-series buckets collection.
 *
 * @tags: [
 *   uses_transactions,
 *   uses_snapshot_read_concern,
 *   does_not_support_viewless_timeseries_yet,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

function incrementOID(oid) {
    const prefix = oid.toString().substr(10, 16);
    const suffix = oid.toString().substr(26, 8);

    const number = parseInt(suffix, 16) + 1;
    const incremented = number.toString(16).padStart(8, '0');

    return ObjectId(prefix + incremented);
}

const session = db.getMongo().startSession();
const sessionDB = session.getDatabase(jsTestName());
assert.commandWorked(sessionDB.dropDatabase());

const tsCollName = "t";
const timeFieldName = 'time';
const metaFieldName = 'meta';
const tsColl = assertDropAndRecreateCollection(
    sessionDB, tsCollName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}});
const bucketsColl = sessionDB.getCollection(TimeseriesTest.getBucketsCollName(tsCollName));

assert.commandWorked(
    tsColl.insert({[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 1}));
assert.commandWorked(
    tsColl.insert({[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 2}));
assert.commandWorked(
    tsColl.insert({[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 3}));

session.startTransaction({readConcern: {level: "snapshot"}});

jsTestLog("Testing findAndModify.");
assert.commandWorked(sessionDB.runCommand({
    findAndModify: bucketsColl.getName(),
    query: {[metaFieldName]: 1},
    update: {$set: {[metaFieldName]: 100}},
}));

assert.commandWorked(sessionDB.runCommand({
    findAndModify: bucketsColl.getName(),
    query: {[metaFieldName]: 100},
    remove: true,
}));

jsTestLog("Testing insert.");
const bogusBucket = bucketsColl.findOne({[metaFieldName]: 3});
assert(bogusBucket);
const id3 = bogusBucket._id;
bogusBucket._id = incrementOID(id3);
assert.commandWorked(bucketsColl.insert(bogusBucket));

jsTestLog("Testing update.");
assert.commandWorked(bucketsColl.update({_id: bogusBucket._id}, {$set: {[metaFieldName]: 4}}));
assert.commandWorked(
    bucketsColl.update({[metaFieldName]: 65},
                       {$set: {"control": bogusBucket.control, "data": bogusBucket.data}},
                       {upsert: true}));

jsTestLog("Testing remove.");
assert.commandWorked(bucketsColl.remove({[metaFieldName]: 65}));
assert.commandWorked(bucketsColl.remove({_id: {$exists: true}}));

jsTestLog("Testing find");
assert.eq(bucketsColl.find().itcount(), 0);

// Insert the bogusBucket again to test aggregate.
assert.commandWorked(bucketsColl.insert(bogusBucket));
assert.eq(bucketsColl.find().itcount(), 1);

jsTestLog("Testing aggregate.");
assert.eq(bucketsColl.aggregate([{$match: {}}]).itcount(), 1);

assert.commandWorked(session.commitTransaction_forTesting());
