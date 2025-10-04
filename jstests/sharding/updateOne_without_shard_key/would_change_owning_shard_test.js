/**
 * Tests the behavior of updates and findAndModifys that would change the owning shard of a
 * document.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_71,
 *  uses_transactions,
 *  uses_multi_shard_transaction,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
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
const shardKey1 = -2;
const shardKey2 = 2;
const docsToInsert = [{_id: 0, x: shardKey1, y: 1}];

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st,
    nss,
    {x: 1},
    [{x: splitPoint}],
    [{query: {x: splitPoint}, shard: st.shard1.shardName}],
);

let testCases = [
    {
        logMessage: "Running WouldChangeOwningShard update without shard key",
        docsToInsert: docsToInsert,
        cmdObj: {
            update: collName,
            updates: [{q: {y: 1}, u: {x: shardKey2, y: 1}}],
        },
        replacementDocTest: true,
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{x: shardKey2, y: 1}],
        expectedResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.updateOne,
    },
    {
        logMessage: "Running WouldChangeOwningShard findAndModify without shard key",
        docsToInsert: docsToInsert,
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            update: {x: shardKey2, y: 1},
        },
        replacementDocTest: true,
        expectedMods: [{x: shardKey2, y: 1}],
        expectedResponse: {lastErrorObject: {n: 1, updatedExisting: true}},
        dbName: dbName,
        collName: collName,
        opType: WriteWithoutShardKeyTestUtil.OperationType.findAndModifyUpdate,
    },
];

const configurations = [
    WriteWithoutShardKeyTestUtil.Configurations.noSession,
    WriteWithoutShardKeyTestUtil.Configurations.sessionNotRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.transaction,
];

const isTxnApiEnabled = FeatureFlagUtil.isEnabled(
    st.s,
    "UpdateDocumentShardKeyUsingTransactionApi",
    undefined /* user */,
    true /* ignoreFCV */,
);

configurations.forEach((config) => {
    let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(st, config);
    testCases.forEach((testCase) => {
        if (
            !isTxnApiEnabled &&
            (config === WriteWithoutShardKeyTestUtil.Configurations.noSession ||
                config === WriteWithoutShardKeyTestUtil.Configurations.sessionNotRetryableWrite)
        ) {
            return;
        }

        WriteWithoutShardKeyTestUtil.runTestWithConfig(conn, testCase, config, testCase.opType);
    });
});

st.stop();
