//
// Tests failed cleanup of orphaned data when we have pending chunks
//

var st = new ShardingTest({shards: 2});

var mongos = st.s0;
var admin = mongos.getDB("admin");
var coll = mongos.getCollection("foo.bar");

assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: st.shard0.shardName}));
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

// Turn off best-effort recipient metadata refresh post-migration commit on both shards because it
// would clean up the pending chunks on migration recipients.
assert.commandWorked(st.shard0.getDB('admin').runCommand(
    {configureFailPoint: 'doNotRefreshRecipientAfterCommit', mode: 'alwaysOn'}));
assert.commandWorked(st.shard1.getDB('admin').runCommand(
    {configureFailPoint: 'doNotRefreshRecipientAfterCommit', mode: 'alwaysOn'}));

jsTest.log("Moving some chunks to shard1...");

assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 1}}));

assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {_id: 0}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {_id: 1}, to: st.shard1.shardName, _waitForDelete: true}));

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

assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {_id: -1}, to: st.shard1.shardName, _waitForDelete: true}));

var metadata =
    st.shard0.getDB("admin").runCommand({getShardVersion: coll + "", fullMetadata: true}).metadata;

printjson(metadata);

assert.eq(metadata.shardVersion.t, 0);
assert.neq(metadata.collVersion.t, 0);
assert.eq(metadata.pending.length, 0);

assert.commandWorked(admin.runCommand(
    {moveChunk: coll + "", find: {_id: 1}, to: st.shard0.shardName, _waitForDelete: true}));

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
