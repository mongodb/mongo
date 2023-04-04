/**
 * Basic tests for running an updateOne without a shard key.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_63,
 *  featureFlagUpdateOneWithoutShardKey,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
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
const setFieldVal = 100;

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 5.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, ns, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

const testCases = [
    {
        logMessage: "Running single set style updateOne without shard key for documents " +
            "on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            update: collName,
            updates: [{q: {y: yFieldVal}, u: {$set: {z: setFieldVal}}}],
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{'z': setFieldVal}],
        expectedResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName,
    },
    {
        logMessage: "Running multiple set style updateOnes without shard key for " +
            "documents on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            update: collName,
            updates: [
                {q: {y: yFieldVal}, u: {$set: {z: setFieldVal}}},
                {q: {y: yFieldVal}, u: {$set: {a: setFieldVal}}}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{'z': setFieldVal}, {'a': setFieldVal}],
        expectedResponse: {n: 2, nModified: 2},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running mixed set style update with shard key and updateOne without" +
            "shard key for documents on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            update: collName,
            updates: [
                {q: {x: xFieldValShard0_1}, u: {$set: {a: setFieldVal}}},
                {q: {y: yFieldVal}, u: {$set: {z: setFieldVal}}},
                {q: {x: xFieldValShard1_1}, u: {$set: {b: setFieldVal}}},
                {q: {y: yFieldVal}, u: {$set: {c: setFieldVal}}}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods:
            [{'a': setFieldVal}, {'z': setFieldVal}, {'b': setFieldVal}, {'c': setFieldVal}],
        expectedResponse: {n: 4, nModified: 4},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running single $set updateOne without shard key for documents " +
            "on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        cmdObj: {
            update: collName,
            updates: [{q: {y: yFieldVal}, u: {$set: {z: setFieldVal}}}],
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{'z': setFieldVal}],
        expectedResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName,
    },
    {
        logMessage: "Running multiple $set updateOnes without shard key for " +
            "documents on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        cmdObj: {
            update: collName,
            updates: [
                {q: {y: yFieldVal}, u: {$set: {z: setFieldVal}}},
                {q: {y: yFieldVal}, u: {$set: {a: setFieldVal}}}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{'z': setFieldVal}, {'a': setFieldVal}],
        expectedResponse: {n: 2, nModified: 2},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running mixed $set update with shard key and updateOne without " +
            "shard key for documents on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        cmdObj: {
            update: collName,
            updates: [
                {q: {x: xFieldValShard0_1}, u: {$set: {a: setFieldVal}}},
                {q: {y: yFieldVal}, u: {$set: {z: setFieldVal}}},
                {q: {x: xFieldValShard0_2}, u: {$set: {b: setFieldVal}}},
                {q: {y: yFieldVal}, u: {$set: {c: setFieldVal}}}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods:
            [{'a': setFieldVal}, {'z': setFieldVal}, {'b': setFieldVal}, {'c': setFieldVal}],
        expectedResponse: {n: 4, nModified: 4},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running single aggregation pipeline updateOne without shard key for" +
            " documents on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            update: collName,
            updates: [{q: {y: yFieldVal}, u: [{$set: {z: setFieldVal}}, {$set: {a: setFieldVal}}]}],
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{'z': setFieldVal}, {'a': setFieldVal}],
        expectedResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName,
    },
    {
        logMessage: "Running multiple aggregation pipeline updateOne without shard key for " +
            "documents on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            update: collName,
            updates: [
                {q: {y: yFieldVal}, u: [{$set: {a: setFieldVal}}, {$set: {b: setFieldVal}}]},
                {q: {y: yFieldVal}, u: [{$set: {c: setFieldVal}}, {$set: {d: setFieldVal}}]}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods:
            [{'a': setFieldVal}, {'b': setFieldVal}, {'c': setFieldVal}, {'d': setFieldVal}],
        expectedResponse: {n: 2, nModified: 2},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running mixed aggregate style update with shard key and updateOne without " +
            "shard key for documents on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],
        cmdObj: {
            update: collName,
            updates: [
                {
                    q: {x: xFieldValShard0_1},
                    u: [{$set: {a: setFieldVal}}, {$set: {b: setFieldVal}}]
                },
                {q: {y: yFieldVal}, u: [{$set: {c: setFieldVal}}, {$set: {d: setFieldVal}}]},
                {
                    q: {x: xFieldValShard1_1},
                    u: [{$set: {e: setFieldVal}}, {$set: {f: setFieldVal}}]
                },
                {q: {y: yFieldVal}, u: [{$set: {g: setFieldVal}}, {$set: {h: setFieldVal}}]}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [
            {'a': setFieldVal},
            {'b': setFieldVal},
            {'c': setFieldVal},
            {'d': setFieldVal},
            {'e': setFieldVal},
            {'f': setFieldVal},
            {'g': setFieldVal},
            {'h': setFieldVal}
        ],
        expectedResponse: {n: 4, nModified: 4},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running single aggregation updateOne without shard key for documents " +
            "on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        cmdObj: {
            update: collName,
            updates: [{q: {y: yFieldVal}, u: [{$set: {a: setFieldVal}}, {$set: {b: setFieldVal}}]}],
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{'a': setFieldVal}, {'b': setFieldVal}],
        expectedResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName,
    },
    {
        logMessage: "Running multiple aggregation updateOnes without shard key for " +
            "documents on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        cmdObj: {
            update: collName,
            updates: [
                {q: {y: yFieldVal}, u: [{$set: {a: setFieldVal}}, {$set: {b: setFieldVal}}]},
                {q: {y: yFieldVal}, u: [{$set: {c: setFieldVal}}, {$set: {d: setFieldVal}}]}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods:
            [{'a': setFieldVal}, {'b': setFieldVal}, {'c': setFieldVal}, {'d': setFieldVal}],
        expectedResponse: {n: 2, nModified: 2},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running mixed aggregation update with shard key and updateOne without " +
            "shard key for documents on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        cmdObj: {
            update: collName,
            updates: [
                {
                    q: {x: xFieldValShard0_1},
                    u: [{$set: {a: setFieldVal}}, {$set: {b: setFieldVal}}]
                },
                {q: {y: yFieldVal}, u: [{$set: {c: setFieldVal}}, {$set: {d: setFieldVal}}]},
                {
                    q: {x: xFieldValShard0_2},
                    u: [{$set: {e: setFieldVal}}, {$set: {f: setFieldVal}}]
                },
                {q: {y: yFieldVal}, u: [{$set: {g: setFieldVal}}, {$set: {h: setFieldVal}}]}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [
            {'a': setFieldVal},
            {'b': setFieldVal},
            {'c': setFieldVal},
            {'d': setFieldVal},
            {'e': setFieldVal},
            {'f': setFieldVal},
            {'g': setFieldVal},
            {'h': setFieldVal}
        ],
        expectedResponse: {n: 4, nModified: 4},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running single replacement style update with shard key and updateOne " +
            "without shard key on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        replacementDocTest: true,  // Replacement tests validate that the final replacement
        // operation was only applied once.
        mustBeInRetryableWriteOrTransaction: true,
        cmdObj: {
            update: collName,
            updates:
                [{q: {y: yFieldVal}, u: {x: xFieldValShard0_2 - 1, y: yFieldVal, a: setFieldVal}}]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{x: xFieldValShard0_2 - 1, y: yFieldVal, a: setFieldVal}],
        expectedResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running multiple replacement style update with shard key and updateOne " +
            "without shard key on the same shard.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard0_2, y: yFieldVal}
        ],

        replacementDocTest: true,  // Replacement tests validate that the final replacement
        // operation was only applied once.
        mustBeInRetryableWriteOrTransaction: true,
        cmdObj: {
            update: collName,
            updates: [
                {q: {y: yFieldVal}, u: {x: xFieldValShard0_2 - 1, y: yFieldVal, a: setFieldVal}},
                {q: {y: yFieldVal}, u: {x: xFieldValShard0_2 - 1, y: yFieldVal, z: setFieldVal}}
            ]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{x: xFieldValShard0_2 - 1, y: yFieldVal, z: setFieldVal}],
        expectedResponse: {n: 2, nModified: 2},
        dbName: dbName,
        collName: collName
    },
    {
        // Due to WouldChangeOwningShard batch size restrictions, we only have a test case for
        // replacement updates of a batch size of 1.
        logMessage: "Running single replacement style update with shard key and updateOne " +
            "without shard key on different shards.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
            {_id: 1, x: xFieldValShard1_1, y: yFieldVal}
        ],

        replacementDocTest: true,  // Replacement tests validate that the final replacement
                                   // operation was only applied once.
        mustBeInRetryableWriteOrTransaction: true,
        cmdObj: {
            update: collName,
            updates: [{q: {y: yFieldVal}, u: {x: xFieldValShard0_2, y: yFieldVal, a: setFieldVal}}]
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{x: xFieldValShard0_2, y: yFieldVal, a: setFieldVal}],
        expectedResponse: {n: 1, nModified: 1},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage:
            "Running a single update where no document matches on the query and {upsert: true}",
        docsToInsert: [],
        cmdObj: {
            update: collName,
            updates: [{q: {y: 5}, u: {_id: 5, x: -1}, upsert: true}],
        },
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{_id: 5, x: -1, y: 5}],
        expectedResponse: {n: 1, nModified: 0, upserted: [{"index": 0, _id: 5}]},
        dbName: dbName,
        collName: collName
    },
    {
        logMessage: "Running a batch update without shard key with an upsert: true update.",
        docsToInsert: [
            {_id: 0, x: xFieldValShard0_1, y: yFieldVal},
        ],
        cmdObj: {
            update: collName,
            updates: [
                {q: {y: yFieldVal}, u: {y: yFieldVal + 1}},
                {q: {y: 6}, u: {x: -1, _id: 6}, upsert: true}
            ],
        },
        mustBeInRetryableWriteOrTransaction: true,
        options: [{ordered: true}, {ordered: false}],
        expectedMods: [{_id: 0, x: xFieldValShard0_1, y: yFieldVal + 1}, {_id: 6, y: 6, x: -1}],
        expectedResponse: {n: 2, nModified: 1, upserted: [{"index": 1, _id: 6}]},
        dbName: dbName,
        collName: collName
    },
];

const configurations = [
    WriteWithoutShardKeyTestUtil.Configurations.noSession,
    WriteWithoutShardKeyTestUtil.Configurations.sessionNotRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.sessionRetryableWrite,
    WriteWithoutShardKeyTestUtil.Configurations.transaction
];

const isTxnApiEnabled = FeatureFlagUtil.isEnabled(
    st.s, "UpdateDocumentShardKeyUsingTransactionApi", undefined /* user */, true /* ignoreFCV */);

configurations.forEach(config => {
    let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(st, config);
    testCases.forEach(testCase => {
        if (!isTxnApiEnabled && testCase.mustBeInRetryableWriteOrTransaction &&
            (config === WriteWithoutShardKeyTestUtil.Configurations.noSession ||
             config === WriteWithoutShardKeyTestUtil.Configurations.sessionNotRetryableWrite)) {
            return;
        }
        WriteWithoutShardKeyTestUtil.runTestWithConfig(
            conn, testCase, config, WriteWithoutShardKeyTestUtil.OperationType.updateOne);
    });
});

st.stop();
})();
