/**
 * Test updateOnes, deleteOnes, and findAndModifys without shard key with Stable API specified
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_70,
 *    uses_transactions,
 *    uses_multi_shard_transaction,
 *    featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

// Make sure we're testing with no implicit session.
TestData.disableImplicitSessions = true;

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
        logMessage: "Running Stable API test for updateOne",
        docsToInsert: docsToInsert,
        cmdObj: {
            update: collName,
            updates: [{q: {y: 1}, u: {$set: {z: 3}}}],
            apiStrict: true,
            apiVersion: "1",
        },
        expectedMods: [
            {'z': 3},
        ],
        expectedResponse: {n: 1, nModified: 1},
        expectedRetryResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.updateOne,
    },
    {
        logMessage: "Running Stable API test for findAndModify update",
        docsToInsert: docsToInsert,
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            update: {$set: {z: 4}},
            apiStrict: true,
            apiVersion: "1",
        },
        expectedMods: [
            {'z': 4},
        ],
        expectedResponse: {lastErrorObject: {n: 1, updatedExisting: true}},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.findAndModifyUpdate,
    },
    {
        logMessage: "Running Stable API test for findAndModify remove",
        docsToInsert: docsToInsert,
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            remove: true,
            apiStrict: true,
            apiVersion: "1",
        },
        expectedResponse: {lastErrorObject: {n: 1}},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.findAndModifyRemove,
    },
    {
        logMessage: "Running Stable API test for deleteOne",
        docsToInsert: docsToInsert,
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: 1}, limit: 1}],
            apiStrict: true,
            apiVersion: "1",
        },
        expectedResponse: {n: 1},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.deleteOne,
    }
];

let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(
    st, WriteWithoutShardKeyTestUtil.Configurations.noSession);
testCases.forEach(testCase => {
    WriteWithoutShardKeyTestUtil.runTestWithConfig(
        conn, testCase, WriteWithoutShardKeyTestUtil.Configurations.noSession, testCase.opType);
});

st.stop();
})();
