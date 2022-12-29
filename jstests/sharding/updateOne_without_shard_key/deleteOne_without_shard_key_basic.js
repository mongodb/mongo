/**
 * Basic tests for running an deleteOne without a shard key.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_63,
 *  featureFlagUpdateOneWithoutShardKey,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

// Make sure we're testing with no implicit session.
TestData.disableImplicitSessions = true;

// 2 shards single node, 1 mongos, 1 config server 3-node
const st = new ShardingTest({});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const splitPoint = 5;
const xFieldValShard0_1 = splitPoint - 1;
const xFieldValShard0_2 = xFieldValShard0_1 - 1;  // A different shard key on shard 0.
const xFieldValShard1_1 = splitPoint + 1;
const yFieldVal = 2;

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 5.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, ns, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

const testCases = [
    {
        logMessage: "Running single deleteOne without shard key for documents " +
            "on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: yFieldVal}, limit: 1}],
        },
        options: [{ordered: true}, {ordered: false}],
        expectedResponse: {n: 1},
        dbName: dbName,
        collName: collName,
    },
    {
        logMessage: "Running multiple deleteOnes without shard key for " +
            "documents on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: yFieldVal}, limit: 1}, {q: {y: yFieldVal}, limit: 1}]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedResponse: {n: 2},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running mixed deleteOnes with shard key and deleteOne without" +
            " shard key for documents on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            delete: collName,
            deletes: [
                {q: {x: xFieldValShard0_1}, limit: 1},
                {q: {y: yFieldVal}, limit: 1},
                {q: {x: xFieldValShard1_1}, limit: 1},
                {q: {y: yFieldVal}, limit: 1}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedResponse: {n: 2},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running single deleteOne without shard key for documents " +
            "on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        cmdObj: {
            delete: collName,
            deletes: [{q: {y: yFieldVal}, limit: 1}],
        },
        options: [{ordered: true}, {ordered: false}],
        expectedResponse: {n: 1},
        dbName: dbName,
        collName: collName,
    },
    {
        logMessage: "Running multiple deleteOnes without shard key for " +
            "documents on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: yFieldVal}, limit: 1}, {q: {y: yFieldVal}, limit: 1}]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedResponse: {n: 2},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running mixed deleteOne with shard key and deleteOne without " +
            "shard key for documents on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        cmdObj: {
            delete: collName,
            deletes: [
                {q: {x: xFieldValShard0_1}, limit: 1},
                {q: {y: yFieldVal}, limit: 1},
                {q: {x: xFieldValShard0_2}, limit: 1},
                {q: {y: yFieldVal}, limit: 1}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedResponse: {n: 2},
        dbName: dbName,
        collName: collName
    },
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
        WriteWithoutShardKeyTestUtil.runTestWithConfig(
            conn, testCase, config, WriteWithoutShardKeyTestUtil.OperationType.deleteOne);
    });
});

st.stop();
})();
