/**
 * Testing the mongos fsyncUnlock functionality.
 * @tags: [
 *   requires_fsync,
 *   featureFlagClusterFsyncLock
 * ]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "collTest";
const ns = dbName + "." + collName;
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    mongosOptions: {setParameter: {featureFlagClusterFsyncLock: true}},
    config: 1
});
const adminDB = st.s.getDB("admin");

jsTest.log("Insert some data.");
const coll = st.s0.getDB(dbName)[collName];
let bulk = coll.initializeUnorderedBulkOp();
for (let i = -50; i < 50; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Create a sharded collection with one chunk on each of the two shards.");
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName, _waitForDelete: true}));

// TODO (SERVER-78149): Fsync Lock Command will be called before Fsync Unlock Command.
let ret = assert.commandFailed(st.s.adminCommand({fsyncUnlock: 1}));
const errmsg = "fsyncUnlock called when not locked";
assert.eq(ret.errmsg.includes(errmsg), true);

st.stop();
}());
