/**
 * Tests that time-series inserts are properly handled when a node steps down from primary and then
 * later steps back up.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */

import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const dbName = "test";
const numColls = 3;

const testDB = function () {
    return replTest.getPrimary().getDB(dbName);
};

const coll = function (num) {
    return testDB()[jsTestName() + "_" + num];
};

const timeFieldName = "time";
const metaFieldName = "m";

const createColl = function (num) {
    assert.commandWorked(
        testDB().createCollection(coll(num).getName(), {
            timeseries: {timeField: timeFieldName, metaField: metaFieldName},
        }),
    );
};

for (let i = 0; i < numColls; i++) {
    createColl(i);
}

const docs = [
    {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: 0},
    {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: 0},
];

for (let i = 0; i < numColls; i++) {
    assert.commandWorked(coll(i).insert(docs[0]));
}

replTest.stepUp(replTest.getSecondary());

// Manually update the bucket for collection 1.
assert.commandWorked(
    getTimeseriesCollForRawOps(testDB(), coll(1)).update({}, {$set: {meta: 1}}, getRawOperationSpec(testDB())),
);
assert.commandWorked(
    getTimeseriesCollForRawOps(testDB(), coll(1)).update({}, {$set: {meta: 0}}, getRawOperationSpec(testDB())),
);

// Drop, recreate, and reinsert the bucket for collection 2.
assert(coll(2).drop());
createColl(2);
assert.commandWorked(coll(2).insert(docs[0]));

// Step back up the original primary.
replTest.stepUp(replTest.getSecondary());

for (let i = 0; i < numColls; i++) {
    assert.commandWorked(coll(i).insert(docs[1]));
}

const checkColl = function (num, numBuckets) {
    jsTestLog("Checking collection " + num);
    assert.docEq(docs, coll(num).find().sort({_id: 1}).toArray());
    const buckets = getTimeseriesCollForRawOps(testDB(), coll(num)).find().rawData().toArray();
    assert.eq(buckets.length, numBuckets, "Expected " + numBuckets + " bucket(s) but found: " + tojson(buckets));
};

// For collection 0, the original bucket should still be usable.
checkColl(0, 1);
// We expect the buckets to be reopened by the new primary when inserting further measurements.
checkColl(1, 1);
checkColl(2, 1);

replTest.stopSet();
