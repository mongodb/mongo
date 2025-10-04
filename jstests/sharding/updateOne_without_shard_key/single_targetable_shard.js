/**
 * Tests that writes without shard key for only a single targetable shard succeed.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_71,
 *  uses_transactions,
 *  uses_multi_shard_transaction,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {WriteWithoutShardKeyTestUtil} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";

// Make sure we're testing with no implicit session.
TestData.disableImplicitSessions = true;

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const splitPoint = 0;
const docsToInsert = [
    {_id: 0, x: -2, y: 1},
    {_id: 1, x: -1, y: 1},
    {_id: 2, x: 1, y: 1},
    {_id: 3, x: 2, y: 1},
];

// Sets up a 2 shard cluster using 'x' as a shard key where one shard owns all chunks.
WriteWithoutShardKeyTestUtil.setupShardedCollection(st, nss, {x: 1}, [{x: splitPoint}], []);

let testCases = [
    {
        logMessage: "Running single shard test for updateOne.",
        docsToInsert: docsToInsert,
        cmdObj: {
            update: collName,
            updates: [{q: {y: 1}, u: {$set: {z: 3}}}],
        },
        expectedMods: [{"z": 3}],
        expectedResponse: {n: 1, nModified: 1},
        options: [{ordered: true}, {ordered: false}],
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.updateOne,
    },
    {
        logMessage: "Running single shard test for findAndModify.",
        docsToInsert: docsToInsert,
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            update: {$set: {z: 4}},
        },
        expectedMods: [{"z": 4}],
        expectedResponse: {lastErrorObject: {n: 1, updatedExisting: true}},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.findAndModifyUpdate,
    },
    {
        logMessage: "Running single shard test for deleteOne.",
        docsToInsert: docsToInsert,
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: 1}, limit: 1}],
        },
        expectedResponse: {n: 1},
        options: [{ordered: true}, {ordered: false}],
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.deleteOne,
    },
];

let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(
    st,
    WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite,
);
testCases.forEach((testCase) => {
    WriteWithoutShardKeyTestUtil.runTestWithConfig(
        conn,
        testCase,
        WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite,
        testCase.opType,
    );
});

const configurations = [
    WriteWithoutShardKeyTestUtil.Configurations.noSession,
    WriteWithoutShardKeyTestUtil.Configurations.sessionNotRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.transaction,
];

configurations.forEach((config) => {
    let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(st, config);
    testCases.forEach((testCase) => {
        WriteWithoutShardKeyTestUtil.runTestWithConfig(conn, testCase, config, testCase.opType);
    });
});

st.stop();
