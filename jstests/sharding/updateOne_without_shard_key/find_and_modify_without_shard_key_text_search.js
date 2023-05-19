/**
 * Test findAndModify without shard key works with $text search predicates.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_71,
 *    uses_transactions,
 *    uses_multi_shard_transaction,
 *    featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const splitPoint = 0;
const docsToInsert = [
    {_id: 0, x: -2, numbers: "one"},
    {_id: 1, x: -1, numbers: "two"},
    {_id: 2, x: 1, numbers: "one"},
    {_id: 3, x: 2, numbers: "two one"},
    {_id: 4, x: 3, numbers: "two three"},
];
const dbConn = st.s.getDB(dbName);
const coll = dbConn.getCollection(collName);

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

assert.commandWorked(coll.insert(docsToInsert));
assert.commandWorked(coll.createIndex({numbers: "text"}));

function runTest(testCase) {
    let res = assert.commandWorked(coll.runCommand(testCase.cmdObj));
    assert.eq(res.lastErrorObject.n, 1);
    assert.eq(res.value.numbers, testCase.expectedResult.numbers);
    if (testCase.projectTextScore) {
        assert(res.value.score);
    } else {
        assert(!res.value.score);
    }
    if (testCase.opType === "update") {
        assert.eq(res.lastErrorObject.updatedExisting, true);
    }
}

let testCases = [
    {
        logMessage: "Running findAndModify update with textScore projection.",
        opType: "update",
        projectTextScore: true,
        cmdObj: {
            findAndModify: collName,
            query: {$text: {$search: "one"}},
            fields: {score: {$meta: "textScore"}},
            update: [{$set: {a: 1}}],
        },
        expectedResult: {numbers: "one"},
    },
    {
        logMessage: "Running findAndModify update with textScore sort.",
        opType: "update",
        cmdObj: {
            findAndModify: collName,
            query: {$text: {$search: "two"}},
            sort: {score: {$meta: "textScore"}},
            update: [{$set: {a: 1}}],
        },
        expectedResult: {numbers: "two"},
    },
    {
        logMessage: "Running findAndModify update with textScore sort and projection.",
        projectTextScore: true,
        opType: "update",
        cmdObj: {
            findAndModify: collName,
            query: {$text: {$search: "two"}},
            sort: {score: {$meta: "textScore"}},
            fields: {score: {$meta: "textScore"}},
            update: [{$set: {a: 1}}],
        },
        expectedResult: {numbers: "two"},
    },
    {
        logMessage: "Running findAndModify remove with textScore projection.",
        opType: "delete",
        projectTextScore: true,
        cmdObj: {
            findAndModify: collName,
            query: {$text: {$search: "one"}},
            fields: {score: {$meta: "textScore"}},
            remove: true,
        },
        expectedResult: {numbers: "one"},
    },
    {
        logMessage: "Running findAndModify remove with textScore sort.",
        opType: "delete",
        cmdObj: {
            findAndModify: collName,
            query: {$text: {$search: "two one"}},
            sort: {score: {$meta: "textScore"}},
            remove: true,
        },
        expectedResult: {numbers: "two one"},
    },
    {
        logMessage: "Running findAndModify remove with textScore sort and projection.",
        projectTextScore: true,
        opType: "delete",
        cmdObj: {
            findAndModify: collName,
            query: {$text: {$search: "two three"}},
            fields: {score: {$meta: "textScore"}},
            sort: {score: {$meta: "textScore"}},
            remove: true,
        },
        expectedResult: {numbers: "two three"},
    },
];

testCases.forEach(testCase => {
    jsTestLog(testCase.logMessage);
    runTest(testCase);
});

st.stop();
})();
