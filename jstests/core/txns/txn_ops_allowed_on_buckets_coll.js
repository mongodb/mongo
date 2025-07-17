/**
 * Tests that transactions are able to write to the time-series buckets collection.
 *
 * @tags: [
 *   uses_transactions,
 *   uses_snapshot_read_concern,
 * ]
 */
import {
    getTimeseriesCollForRawOps,
    kRawOperationSpec
} from "jstests/core/libs/raw_operation_utils.js";
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

assert.commandWorked(
    tsColl.insert({[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 1}));
assert.commandWorked(
    tsColl.insert({[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 2}));
assert.commandWorked(
    tsColl.insert({[timeFieldName]: ISODate("2023-08-01T00:00:00.000Z"), [metaFieldName]: 3}));

session.startTransaction({readConcern: {level: "snapshot"}});

jsTestLog("Testing findAndModify.");
assert.commandWorked(sessionDB.runCommand({
    findAndModify: getTimeseriesCollForRawOps(tsColl).getName(),
    query: {[metaFieldName]: 1},
    update: {$set: {[metaFieldName]: 100}},
    ...kRawOperationSpec,
}));

assert.commandWorked(sessionDB.runCommand({
    findAndModify: getTimeseriesCollForRawOps(tsColl).getName(),
    query: {[metaFieldName]: 100},
    remove: true,
    ...kRawOperationSpec,
}));

jsTestLog("Testing insert.");
const bogusBucket = getTimeseriesCollForRawOps(tsColl).findOne(
    {[metaFieldName]: 3}, null, null, null, null, true /* rawData */);
assert(bogusBucket);
const id3 = bogusBucket._id;
bogusBucket._id = incrementOID(id3);
assert.commandWorked(getTimeseriesCollForRawOps(tsColl).insert(bogusBucket, kRawOperationSpec));

jsTestLog("Testing update.");
assert.commandWorked(getTimeseriesCollForRawOps(tsColl).update(
    {_id: bogusBucket._id}, {$set: {[metaFieldName]: 4}}, kRawOperationSpec));
assert.commandWorked(getTimeseriesCollForRawOps(tsColl).update(
    {[metaFieldName]: 65},
    {$set: {"control": bogusBucket.control, "data": bogusBucket.data}},
    {upsert: true, ...kRawOperationSpec}));

jsTestLog("Testing remove.");
assert.commandWorked(
    getTimeseriesCollForRawOps(tsColl).remove({[metaFieldName]: 65}, kRawOperationSpec));
assert.commandWorked(
    getTimeseriesCollForRawOps(tsColl).remove({_id: {$exists: true}}, kRawOperationSpec));

jsTestLog("Testing find");
assert.eq(getTimeseriesCollForRawOps(tsColl).find().rawData().itcount(), 0);

// Insert the bogusBucket again to test aggregate.
assert.commandWorked(getTimeseriesCollForRawOps(tsColl).insert(bogusBucket, kRawOperationSpec));
assert.eq(getTimeseriesCollForRawOps(tsColl).find().rawData().itcount(), 1);

jsTestLog("Testing aggregate.");
assert.eq(getTimeseriesCollForRawOps(tsColl).aggregate([{$match: {}}], kRawOperationSpec).itcount(),
          1);

assert.commandWorked(session.commitTransaction_forTesting());
