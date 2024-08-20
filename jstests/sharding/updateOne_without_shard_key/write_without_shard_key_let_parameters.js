/**
 * Test updateOnes, deleteOnes, and findAndModifys without shard key with let parameters.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_71,
 *    uses_transactions,
 *    uses_multi_shard_transaction,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    WriteWithoutShardKeyTestUtil
} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const splitPoint = 0;
const docsToInsert =
    [{_id: 0, x: -2, y: 1}, {_id: 1, x: -1, y: 1}, {_id: 2, x: 1, y: 1}, {_id: 3, x: 2, y: 1}];

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

let testCases = [
    {
        logMessage: "Running updateOne using let parameters",
        docsToInsert: docsToInsert,
        cmdObj: {
            update: collName,
            updates: [{q: {$expr: {$eq: ["$y", "$$myVal"]}}, u: {$set: {z: 3}}}],
            let : {myVal: 1},
        },
        expectedMods: [
            {'z': 3},
        ],
        expectedResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.updateOne,
    },
    {
        logMessage: "Running findAndModify update using let parameters",
        docsToInsert: docsToInsert,
        cmdObj: {
            findAndModify: collName,
            query: {$expr: {$eq: ["$y", "$$myVal"]}},
            update: {$set: {z: 3}},
            let : {myVal: 1},
        },
        expectedMods: [
            {'z': 3},
        ],
        expectedResponse: {lastErrorObject: {n: 1}},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.findAndModifyUpdate,
    },
    {
        logMessage: "Running findAndModify remove using let parameters",
        docsToInsert: docsToInsert,
        cmdObj: {
            findAndModify: collName,
            query: {$expr: {$eq: ["$y", "$$myVal"]}},
            remove: true,
            let : {myVal: 1},
        },
        expectedResponse: {lastErrorObject: {n: 1}},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.findAndModifyRemove,
    },
    {
        logMessage: "Running deleteOne using let parameters",
        docsToInsert: docsToInsert,
        cmdObj: {
            delete: collName,
            deletes: [{q: {$expr: {$eq: ["$y", "$$myVal"]}}, limit: 1}],
            let : {myVal: 1},
        },
        expectedResponse: {n: 1},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.deleteOne,
    }
];

let configurations = [
    WriteWithoutShardKeyTestUtil.Configurations.noSession,
    WriteWithoutShardKeyTestUtil.Configurations.sessionNotRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.transaction
];

configurations.forEach(config => {
    let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(st, config);
    testCases.forEach(testCase => {
        WriteWithoutShardKeyTestUtil.runTestWithConfig(conn, testCase, config, testCase.opType);
    });
});

st.stop();
