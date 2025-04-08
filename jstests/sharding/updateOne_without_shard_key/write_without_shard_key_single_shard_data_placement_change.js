/**
 * Test writes without shard key in a transaction with snapshot read concern fails during data
 * placement change.
 *
 * @tags: [
 *    requires_fcv_71,
 *    requires_sharding,
 *    uses_transactions,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    WriteWithoutShardKeyTestUtil
} from "jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js";

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const collName2 = "testColl2";
const nss = dbName + "." + collName;
const splitPoint = 0;
const docsToInsert = [
    {_id: 1, x: -1, y: 1},
    {_id: 2, x: 1, y: 2},
];
const dbConn = st.s.getDB(dbName);
const coll = dbConn.getCollection(collName);

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

assert.commandWorked(coll.insert(docsToInsert));

function runTest(testCase) {
    const session = st.s.startSession();
    session.startTransaction({readConcern: {level: "snapshot"}});
    session.getDatabase(dbName).getCollection(collName2).insert({x: 1});
    let hangDonorAtStartOfRangeDel =
        configureFailPoint(st.rs1.getPrimary(), "suspendRangeDeletion");

    // Move all chunks for testDb.testColl to shard0.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: nss, find: {x: 0}, to: st.shard0.shardName}));
    hangDonorAtStartOfRangeDel.wait();

    // This write cmd MUST fail, the data moved to another shard, we can't try on shard0 nor
    // shard1 with the original clusterTime of the transaction.
    assert.commandFailedWithCode(session.getDatabase(dbName).runCommand(testCase.cmdObj),
                                 ErrorCodes.MigrationConflict);

    hangDonorAtStartOfRangeDel.off();

    // Reset the chunk distribution for the next test.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: nss, find: {x: 0}, to: st.shard1.shardName}));
}

let testCases = [
    {
        logMessage: "Running updateOne test",
        cmdObj: {
            update: collName,
            updates: [{q: {y: 2}, u: {$inc: {z: 1}}}],
        },
    },
    {
        logMessage: "Running findAndModify test",
        cmdObj: {
            findAndModify: collName,
            query: {y: 2},
            update: {$inc: {z: 1}},
        }
    },
    {
        logMessage: "Running deleteOne test.",
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: 2}, limit: 1}],
        },
    }
];

testCases.forEach(testCase => {
    jsTestLog(testCase.logMessage);
    runTest(testCase);
});

st.stop();
