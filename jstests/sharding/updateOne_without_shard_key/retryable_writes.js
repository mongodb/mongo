/**
 * Test retryable updateOnes, deleteOnes, and findAndModifys without shard key.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_63,
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
        logMessage: "Running retryable write test for updateOne",
        docsToInsert: docsToInsert,
        cmdObj: {
            update: collName,
            updates: [{q: {y: 1}, u: {$set: {z: 3}}}],
            stmtId: NumberInt(1),
        },
        retryableWriteTest: true,
        expectedMods: [
            {'z': 3},
        ],
        expectedResponse: {n: 1, nModified: 1},
        expectedRetryResponse: {n: 1, nModified: 1, retriedStmtIds: [1]},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.updateOne,
    },
    {
        logMessage: "Running retryable write test for findAndModify update",
        docsToInsert: docsToInsert,
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            update: {$set: {z: 4}},
            stmtId: NumberInt(2),
        },
        retryableWriteTest: true,
        expectedMods: [
            {'z': 4},
        ],
        expectedResponse: {lastErrorObject: {n: 1, updatedExisting: true}},
        expectedRetryResponse: {lastErrorObject: {n: 1, updatedExisting: true}, retriedStmtId: 2},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.findAndModifyUpdate,
    },
    {
        logMessage: "Running retryable write test for findAndModify remove",
        docsToInsert: docsToInsert,
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            remove: true,
            stmtId: NumberInt(3),
        },
        retryableWriteTest: true,
        expectedResponse: {lastErrorObject: {n: 1}},
        expectedRetryResponse: {lastErrorObject: {n: 1}, retriedStmtId: 3},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.findAndModifyRemove,
    },
    {
        logMessage: "Running retryable write test for deleteOne",
        docsToInsert: docsToInsert,
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: 1}, limit: 1}],
            stmtId: NumberInt(4),
        },
        retryableWriteTest: true,
        expectedResponse: {n: 1},
        expectedRetryResponse: {n: 1, retriedStmtIds: [4]},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.deleteOne,
    }
];

let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(
    st, WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite);
testCases.forEach(testCase => {
    WriteWithoutShardKeyTestUtil.runTestWithConfig(
        conn,
        testCase,
        WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite,
        testCase.opType);
});

st.stop();
})();
