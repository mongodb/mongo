/**
 * Test writes without shard key uses the hint provided in the original query.
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
    {_id: 0, x: -2, y: 1},
    {_id: 1, x: -1, y: 1},
    {_id: 2, x: 1, y: 1},
    {_id: 3, x: 2, y: 1},
];
const dbConn = st.s.getDB(dbName);
const coll = dbConn.getCollection(collName);

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

assert.commandWorked(coll.insert(docsToInsert));

// Create a sparse index on 'a' which has no documents. We use a hint for a sparse index to assert
// that we use the expected index, which in this case should match no documents even though we have
// potentially matching documents.
assert.commandWorked(coll.createIndex({a: 1}, {sparse: true}));

function runTest(testCase) {
    let res = assert.commandWorked(coll.runCommand(testCase.cmdObj));
    if (testCase.op == "update") {
        assert.eq(res.nModified, 0);
        assert.eq(res.n, 0);
    } else if (testCase.op == "findAndModify") {
        assert.eq(res.lastErrorObject.n, 0);
    } else {
        assert.eq(res.n, 0);
    }
}

let testCases = [
    {
        logMessage: "Running updateOne with hint.",
        op: "update",
        cmdObj: {
            update: collName,
            updates: [{q: {y: 1}, u: {$set: {z: 3}}, hint: {a: 1}}],
        },
    },
    {
        logMessage: "Running findAndModify with hint.",
        op: "findAndModify",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            update: {$set: {z: 4}},
            hint: {a: 1},
        },
    },
    {
        logMessage: "Running deleteOne with hint.",
        op: "delete",
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: 1}, limit: 1, hint: {a: 1}}],
        },
    }
];

testCases.forEach(testCase => {
    jsTestLog(testCase.logMessage);
    runTest(testCase);
});

st.stop();
})();
