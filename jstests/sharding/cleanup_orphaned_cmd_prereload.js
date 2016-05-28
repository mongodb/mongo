//
// Tests failed cleanup of orphaned data when we have pending chunks
//

var st = new ShardingTest({shards: 2});

var mongos = st.s0;
var admin = mongos.getDB("admin");
var shards = mongos.getCollection("config.shards").find().toArray();
var coll = mongos.getCollection("foo.bar");

assert(admin.runCommand({enableSharding: coll.getDB() + ""}).ok);
printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: shards[0]._id}));
assert(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}).ok);

jsTest.log("Moving some chunks to shard1...");

assert(admin.runCommand({split: coll + "", middle: {_id: 0}}).ok);
assert(admin.runCommand({split: coll + "", middle: {_id: 1}}).ok);

assert(
    admin
        .runCommand({moveChunk: coll + "", find: {_id: 0}, to: shards[1]._id, _waitForDelete: true})
        .ok);
assert(
    admin
        .runCommand({moveChunk: coll + "", find: {_id: 1}, to: shards[1]._id, _waitForDelete: true})
        .ok);

var metadata =
    st.shard1.getDB("admin").runCommand({getShardVersion: coll + "", fullMetadata: true}).metadata;

printjson(metadata);

assert.eq(metadata.pending[0][0]._id, 1);
assert.eq(metadata.pending[0][1]._id, MaxKey);

jsTest.log("Ensuring we won't remove orphaned data in pending chunk...");

assert(!st.shard1.getDB("admin")
            .runCommand({cleanupOrphaned: coll + "", startingFromKey: {_id: 1}})
            .stoppedAtKey);

jsTest.log("Moving some chunks back to shard0 after empty...");

admin.runCommand({moveChunk: coll + "", find: {_id: -1}, to: shards[1]._id, _waitForDelete: true});

var metadata =
    st.shard0.getDB("admin").runCommand({getShardVersion: coll + "", fullMetadata: true}).metadata;

printjson(metadata);

assert.eq(metadata.shardVersion.t, 0);
assert.neq(metadata.collVersion.t, 0);
assert.eq(metadata.pending.length, 0);

assert(
    admin
        .runCommand({moveChunk: coll + "", find: {_id: 1}, to: shards[0]._id, _waitForDelete: true})
        .ok);

var metadata =
    st.shard0.getDB("admin").runCommand({getShardVersion: coll + "", fullMetadata: true}).metadata;

printjson(metadata);
assert.eq(metadata.shardVersion.t, 0);
assert.neq(metadata.collVersion.t, 0);
assert.eq(metadata.pending[0][0]._id, 1);
assert.eq(metadata.pending[0][1]._id, MaxKey);

jsTest.log("Ensuring again we won't remove orphaned data in pending chunk...");

assert(!st.shard0.getDB("admin")
            .runCommand({cleanupOrphaned: coll + "", startingFromKey: {_id: 1}})
            .stoppedAtKey);

jsTest.log("Checking that pending chunk is promoted on reload...");

assert.eq(null, coll.findOne({_id: 1}));

var metadata =
    st.shard0.getDB("admin").runCommand({getShardVersion: coll + "", fullMetadata: true}).metadata;

printjson(metadata);
assert.neq(metadata.shardVersion.t, 0);
assert.neq(metadata.collVersion.t, 0);
assert.eq(metadata.chunks[0][0]._id, 1);
assert.eq(metadata.chunks[0][1]._id, MaxKey);

st.printShardingStatus();

jsTest.log("DONE!");

st.stop();
