/**
 * Tests the behavior of setIndexCommitQuorum when routed through a mongos.
 *
 * @tags: [requires_fcv_50]
 */
(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/sharding/libs/create_sharded_collection_util.js");

const st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 2}});
const dbName = "testDB";
const collName = "coll";
const mongosDB = st.s0.getDB(dbName);
const shard0 = st.shard0.rs.getPrimary();

const generateCreateIndexThread = (host, dbName, collName) => {
    return new Thread(function(host, dbName, collName) {
        const mongos = new Mongo(host);
        const db = mongos.getDB(dbName);
        // Use the index builds coordinator for a two-phase index build.
        assert.commandWorked(db.runCommand({
            createIndexes: collName,
            indexes: [{key: {idxKey: 1}, name: 'idxKey_1'}],
            commitQuorum: "majority"
        }));
    }, host, dbName, collName);
};

// When setIndexCommitQuorum is sent by the mongos, the response follows the format:
// {
//  raw: {
//      "<shard0URL>": {
//          <response from setIndexCommitQuorum being run on shard>
//      },
//      "<shard1URL>": {....}
//  }
//  ok: <>
//  ...
// }
//
// Returns the shard's corresponding entry in the "raw" field of the mongosResponse.
const extractShardReplyFromResponse = (shard, mongosResponse) => {
    assert(mongosResponse.raw);
    const shardURL = shard.rs.getURL();
    return mongosResponse.raw[shardURL];
};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

jsTest.log("Testing setIndexCommitQuorum from a mongos can succeed");
// Create a sharded collection where only shard0 owns chunks.
CreateShardedCollectionUtil.shardCollectionWithChunks(mongosDB[collName], {_id: 1}, [
    {min: {_id: MinKey}, max: {_id: MaxKey}, shard: st.shard0.shardName},
]);
assert.commandWorked(mongosDB[collName].insert({_id: 1}));

let createIndexThread = generateCreateIndexThread(st.s0.host, dbName, collName);
let createIndexFailpoint = configureFailPoint(shard0, "hangAfterIndexBuildFirstDrain");
createIndexThread.start();
createIndexFailpoint.wait();

// Confirm mongos succeeds when only one shard owns chunks (and data for the collection).
assert.commandWorked(mongosDB.runCommand(
    {setIndexCommitQuorum: collName, indexNames: ["idxKey_1"], commitQuorum: 2}));
createIndexFailpoint.off();
createIndexThread.join();
assert(mongosDB[collName].drop());

jsTest.log("Testing setIndexCommitQuorum from a mongos fails but reports partial success");
// In the event that setIndexCommitQuorum succeeds on one shard, but fails on another, the mongos
// setIndexCommitQuorum should return an error and include information on what shards
// failed/succeeded.

// Create a sharded collection where both shards own chunks, but only shard0 bears data.
CreateShardedCollectionUtil.shardCollectionWithChunks(mongosDB[collName], {_id: 1}, [
    {min: {_id: MinKey}, max: {_id: 10}, shard: st.shard0.shardName},
    {min: {_id: 10}, max: {_id: MaxKey}, shard: st.shard1.shardName},
]);
assert.commandWorked(mongosDB[collName].insert({_id: 1}));
createIndexThread = generateCreateIndexThread(st.s0.host, dbName, collName);
createIndexFailpoint = configureFailPoint(shard0, "hangAfterIndexBuildFirstDrain");

createIndexThread.start();
createIndexFailpoint.wait();

const res = assert.commandFailedWithCode(
    mongosDB.runCommand(
        {setIndexCommitQuorum: collName, indexNames: ["idxKey_1"], commitQuorum: 2}),
    ErrorCodes.IndexNotFound);

// Confirm the mongos reports shard0 successfully set its index commit quorum despite the mongos
// command failing.
const shard0Reply = extractShardReplyFromResponse(st.shard0, res);
assert.eq(shard0Reply.ok, 1);

// Confirm shard1 caused the command to fail since it didn't actually own any data for the
// collection.
const shard1Reply = extractShardReplyFromResponse(st.shard1, res);
assert.eq(shard1Reply.ok, 0);
assert.eq(shard1Reply.code, ErrorCodes.IndexNotFound);

createIndexFailpoint.off();
createIndexThread.join();

st.stop();
}());
