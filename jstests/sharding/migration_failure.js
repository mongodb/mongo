//
// Tests that migration failures before and after commit correctly roll back
// when possible
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var shards = mongos.getCollection("config.shards").find().toArray();
    var coll = mongos.getCollection("foo.bar");

    assert(admin.runCommand({enableSharding: coll.getDB() + ""}).ok);
    printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: shards[0]._id}));
    assert(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}).ok);
    assert(admin.runCommand({split: coll + "", middle: {_id: 0}}).ok);

    st.printShardingStatus();

    jsTest.log("Testing failed migrations...");

    var version = null;
    var failVersion = null;

    // failMigrationCommit
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failMigrationCommit', mode: 'alwaysOn'}));

    version = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()});

    assert.commandFailed(
        admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: shards[1]._id}));

    failVersion = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()});

    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failMigrationCommit', mode: 'off'}));

    // failApplyChunkOps and failCommitMigrationCommand
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failApplyChunkOps', mode: 'alwaysOn'}));
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failCommitMigrationCommand', mode: 'alwaysOn'}));

    version = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()});

    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: shards[1]._id}));

    failVersion = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()});

    assert.neq(version.global, failVersion.global);

    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failApplyChunkOps', mode: 'off'}));
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failCommitMigrationCommand', mode: 'off'}));

    st.stop();

})();
