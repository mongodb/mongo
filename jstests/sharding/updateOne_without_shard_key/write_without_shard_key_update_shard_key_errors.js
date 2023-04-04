/**
 * Test that updateOne and findAndModify without shard key fails to update a document shard key if
 * it was not run as a retryable write or transaction.
 *
 * TODO: SERVER-67429 Remove or amend this test since we can update document shard key outside of a
 * retryable write or transaction.
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

assert.commandWorked(st.getDB(dbName).getCollection(collName).insert(docsToInsert));

const updateShardKeyErrMsg = "Must run update to shard key field in a multi-statement transaction";

let testCases = [
    {
        logMessage: "Running update document shard key error test for updateOne.",
        cmdObj: {
            update: collName,
            updates: [{q: {y: 1}, u: {x: 5, y: 1}}],
        },
    },
    {
        logMessage: "Running update document shard key error test for findAndModify",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            update: {x: 5, y: 1},
        },
    },
];

const configurations = [
    WriteWithoutShardKeyTestUtil.Configurations.noSession,
    WriteWithoutShardKeyTestUtil.Configurations.sessionNotRetryableWrite,
];

configurations.forEach(config => {
    testCases.forEach(testCase => {
        jsTestLog(testCase.logMessage);
        let conn = WriteWithoutShardKeyTestUtil.getClusterConnection(st, config);
        let dbConn;
        if (config === WriteWithoutShardKeyTestUtil.Configurations.noSession) {
            dbConn = conn.getDB(dbName);
        } else {
            dbConn = conn.getDatabase(dbName);
        }

        let res = assert.commandFailedWithCode(dbConn.runCommand(testCase.cmdObj),
                                               ErrorCodes.IllegalOperation);
        let errmsg;
        if (res.writeErrors) {
            errmsg = res.writeErrors[0].errmsg;
        } else {
            errmsg = res.errmsg;
        }
        assert(errmsg.includes(updateShardKeyErrMsg));
    });
});

st.stop();
})();
