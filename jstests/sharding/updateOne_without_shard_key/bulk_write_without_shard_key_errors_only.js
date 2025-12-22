/**
 * Tests that bulk write upserts without shard key honor the errorsOnly parameter.
 *
 * @tags: [
 *    requires_sharding,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {WriteWithoutShardKeyTestUtil} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";
import {cursorSizeValidator, summaryFieldsValidator} from "jstests/libs/bulk_write_utils.js";

const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const db = st.getDB(dbName);
const coll = db.getCollection(collName);
const splitPoint = 0;
const docsToInsert = [
    {_id: 0, x: -2, y: 1},
    {_id: 1, x: -1, y: 1},
    {_id: 2, x: 1, y: 1},
    {_id: 3, x: 2, y: 1},
];

function runCommandAndVerify(testCase) {
    // Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
    // splitPoint and Shard 1 splitPoint >= 0.
    WriteWithoutShardKeyTestUtil.setupShardedCollection(
        st,
        nss,
        {x: 1},
        [{x: splitPoint}],
        [{query: {x: splitPoint}, shard: st.shard1.shardName}],
    );

    assert.commandWorked(coll.insertMany(docsToInsert));

    const bulkCommand = {
        bulkWrite: 1,
        nsInfo: [{ns: nss}],
        ordered: false, // So we don't abort on the first error.
        ops: testCase.ops,
        errorsOnly: testCase.errorsOnly,
        lsid: {id: UUID()},
        txnNumber: NumberLong(1),
    };
    const res = db.adminCommand(bulkCommand);

    assert.commandWorked(res, "bulkWrite command response: " + tojson(res));
    cursorSizeValidator(res, testCase.cursorSize);
    summaryFieldsValidator(res, testCase.summaryFields);

    assert(coll.drop());
}

const testCases = [
    {
        logMessage: "Running bulkWrite with errorsOnly: true, all success",
        errorsOnly: true,
        ops: [
            {update: 0, multi: false, filter: {_id: 5}, updateMods: {x: 0, y: 2}, upsert: true},
            {update: 0, multi: false, filter: {_id: 7}, updateMods: {x: 4, y: 6}, upsert: true},
        ],
        cursorSize: 0,
        summaryFields: {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 2},
    },
    {
        logMessage: "Running bulkWrite with errorsOnly: true, errors",
        errorsOnly: true,
        ops: [
            {update: 0, multi: false, filter: {_id: 5}, updateMods: {x: 0, y: 2, _id: 2}, upsert: true},
            {update: 0, multi: false, filter: {_id: 7}, updateMods: {x: 0, y: 2}, upsert: true},
        ],
        cursorSize: 1,
        summaryFields: {nErrors: 1, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 1},
    },
    {
        logMessage: "Running bulkWrite with errorsOnly: false, all success",
        errorsOnly: false,
        ops: [
            {update: 0, multi: false, filter: {_id: 5}, updateMods: {x: 0, y: 2}, upsert: true},
            {update: 0, multi: false, filter: {_id: 7}, updateMods: {x: 4, y: 6}, upsert: true},
        ],
        cursorSize: 2,
        summaryFields: {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 2},
    },
];

testCases.forEach((testCase) => {
    jsTest.log(testCase.logMessage);
    runCommandAndVerify(testCase);
});

st.stop();
