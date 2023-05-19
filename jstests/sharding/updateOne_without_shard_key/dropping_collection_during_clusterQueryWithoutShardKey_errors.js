/**
 * Verify that running _clusterQueryWithoutShardKey on a collection that is not sharded errors
 * with NamespaceNotSharded.
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
load("jstests/libs/fail_point_util.js");

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const splitPoint = 0;
const docsToInsert =
    [{_id: 0, x: -2, y: 1}, {_id: 1, x: -1, y: 2}, {_id: 3, x: 1, y: 3}, {_id: 4, x: 2, y: 1}];
const testColl = st.getDB(dbName).getCollection(collName);

const findAndModifyThread = new Thread((host, dbName, collName) => {
    const conn = new Mongo(host);
    const cmdObj = {
        findAndModify: collName,
        query: {y: 1},
        update: {y: 5},
    };
    assert.commandFailedWithCode(conn.getDB(dbName).getCollection(collName).runCommand(cmdObj),
                                 ErrorCodes.NamespaceNotSharded);
    assert.eq(null, conn.getDB(dbName).getCollection(collName).findOne({y: 5}));
}, st.s.host, dbName, collName);

const updateOneThread = new Thread((host, dbName, collName) => {
    const conn = new Mongo(host);
    const cmdObj = {update: collName, updates: [{q: {y: 2}, u: {$set: {z: 0}}}]};
    assert.commandFailedWithCode(conn.getDB(dbName).getCollection(collName).runCommand(cmdObj),
                                 ErrorCodes.NamespaceNotSharded);
    assert.eq(null, conn.getDB(dbName).getCollection(collName).findOne({z: 0}));
}, st.s.host, dbName, collName);

const deleteOneThread = new Thread((host, dbName, collName) => {
    const conn = new Mongo(host);
    const cmdObj = {
        delete: collName,
        deletes: [{q: {y: 3}, limit: 1}],
    };
    assert.commandFailedWithCode(conn.getDB(dbName).getCollection(collName).runCommand(cmdObj),
                                 ErrorCodes.NamespaceNotSharded);
    assert.neq(null, conn.getDB(dbName).getCollection(collName).findOne({y: 3}));
}, st.s.host, dbName, collName);

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 x >= splitPoint.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

let hangQueryFp = configureFailPoint(st.s, "hangBeforeMetadataRefreshClusterQuery");
assert.commandWorked(testColl.insert(docsToInsert));

findAndModifyThread.start();
updateOneThread.start();
deleteOneThread.start();
hangQueryFp.wait(3);

// Drop sharded collection.
assert.commandWorked(st.s.getDB(dbName).runCommand({drop: collName}));

// Create unsharded collection.
assert.commandWorked(st.s.getDB(dbName).runCommand({create: collName}));
assert.commandWorked(testColl.insert(docsToInsert));

hangQueryFp.off();

findAndModifyThread.join();
updateOneThread.join();
deleteOneThread.join();

st.stop();
})();
