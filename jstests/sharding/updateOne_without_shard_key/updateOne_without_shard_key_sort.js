/**
 * Test updateOne without shard key with a sort order provided.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_81,
 *    uses_transactions,
 *    uses_multi_shard_transaction,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    WriteWithoutShardKeyTestUtil
} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";

// Make sure we're testing with no implicit session.
TestData.disableImplicitSessions = true;

// 2 single node shards, 1 mongos, 1 3-node config server.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const splitPoint = 0;
const docsToInsert =
    [{_id: 0, x: -2, y: 1}, {_id: 1, x: -1, y: 1}, {_id: 3, x: 1, y: 1}, {_id: 4, x: 2, y: 1}];

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

let testCases = [
    {
        logMessage: "Running updateOne with an ascending sort for 'x'",
        docsToInsert: docsToInsert,
        cmdObj: {update: collName, updates: [{q: {y: 1}, u: {$set: {z: 4}}, sort: {x: 1}}]},
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [
            {'z': 4},
        ],
        expectedResponse: {n: 1, nModified: 1},
        updatedDocId: {'_id': 0},
        dbName: dbName,
        collName: collName,
    },
    {
        logMessage: "Running updateOne with an descending sort for 'x'",
        docsToInsert: docsToInsert,
        cmdObj: {update: collName, updates: [{q: {y: 1}, u: {$set: {z: 4}}, sort: {x: -1}}]},
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [
            {'z': 4},
        ],
        expectedResponse: {n: 1, nModified: 1},
        updatedDocId: {'_id': 4},
        dbName: dbName,
        collName: collName,
    },
];

const configurations = [
    WriteWithoutShardKeyTestUtil.Configurations.noSession,
    WriteWithoutShardKeyTestUtil.Configurations.sessionNotRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.transaction
];

configurations.forEach(config => {
    let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(st, config);
    testCases.forEach(testCase => {
        WriteWithoutShardKeyTestUtil.runTestWithConfig(
            conn, testCase, config, WriteWithoutShardKeyTestUtil.OperationType.updateOne);
    });
});

st.stop();
