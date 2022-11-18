/**
 * Ensure that schema validation rules applied after a collection has been populated, does not
 * inhibit chunk migration for the data that existed before the rules were applied.
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/sharding/libs/create_sharded_collection_util.js");

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}});
const dbName = "test";
const collName = "foo";
const ns = "test.foo";
const testColl = st.s.getCollection(ns);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
CreateShardedCollectionUtil.shardCollectionWithChunks(testColl, {x: 1}, [
    {min: {x: MinKey}, max: {x: 50}, shard: st.shard0.shardName},
    {min: {x: 50}, max: {x: MaxKey}, shard: st.shard0.shardName},
]);

for (let i = 0; i < 100; i++) {
    assert.commandWorked(testColl.insert({x: i, name: "A"}));
}
assert.eq(100, testColl.find().itcount());

assert.commandWorked(testColl.runCommand({collMod: "foo", validator: {name: {$type: "int"}}}));

let failpoint = configureFailPoint(st.shard1, "migrateThreadHangAtStep4");

const awaitResult = startParallelShell(
    funWithArgs(function(ns, toShardName) {
        assert.commandWorked(
            db.adminCommand({moveChunk: ns, find: {x: 50}, to: toShardName, _waitForDelete: true}));
    }, ns, st.shard1.shardName), st.s.port);

failpoint.wait();

for (let i = 100; i < 200; i++) {
    assert.commandWorked(testColl.runCommand(
        {insert: collName, documents: [{x: i, name: "B"}], bypassDocumentValidation: true}));
}

for (let i = 50; i < 75; ++i) {
    assert.commandWorked(testColl.remove({x: i}));
}

failpoint.off();

awaitResult();

const donor = st.shard0.rs.getPrimary().getDB(dbName);
const recipient = st.shard1.rs.getPrimary().getDB(dbName);
assert.eq(50,
          donor.foo.find().itcount(),
          "Number of documents on the donor shard after moveChunk is incorrect.");
assert.eq(125,
          recipient.foo.find().itcount(),
          "Number of documents on the recipient shard after moveChunk is incorrect.");
assert.eq(175, testColl.find().itcount(), "Number of total documents is incorrect");

st.stop();
})();
