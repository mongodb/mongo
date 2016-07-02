//
// Tests that migration failures before and after commit correctly recover when possible.
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

    var oldVersion = null;
    var newVersion = null;

    // failMigrationCommit -- this creates an error that aborts the migration before the commit
    // migration command is sent.
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failMigrationCommit', mode: 'alwaysOn'}));

    oldVersion = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()}).global;

    assert.commandFailed(
        admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: shards[1]._id}));

    newVersion = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()}).global;

    assert.eq(oldVersion.t,
              newVersion.t,
              "The shard version major value should not change after a failed migration");
    assert.eq(oldVersion.i,
              newVersion.i,
              "The shard version minor value should not change after a failed migration");

    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failMigrationCommit', mode: 'off'}));

    // failApplyChunkOps and failCommitMigrationCommand -- these mimic migration commit commands
    // returning errors (e.g. network), whereupon the config server is queried to determine whether
    // the commit was successful.
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failApplyChunkOps', mode: 'alwaysOn'}));
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failCommitMigrationCommand', mode: 'alwaysOn'}));

    // Run a migration where there will still be chunks in the collection remaining on the shard
    // afterwards. This will cause the collection's shardVersion to be bumped higher.
    oldVersion = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()}).global;

    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 1}, to: shards[1]._id}));

    newVersion = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()}).global;

    assert.lt(
        oldVersion.t, newVersion.t, "The major value in the shard version should have increased");
    assert.eq(1, newVersion.i, "The minor value in the shard version should be 1");

    // Run a migration to move off the shard's last chunk in the collection. The collection's
    // shardVersion will be reset.
    oldVersion = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()}).global;

    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: -1}, to: shards[1]._id}));

    newVersion = st.shard0.getDB("admin").runCommand({getShardVersion: coll.toString()}).global;

    assert.gt(oldVersion.t,
              newVersion.t,
              "The version prior to the migration should be greater than the reset value");

    assert.eq(
        0, newVersion.t, "The shard version should have reset, but the major value is not zero");
    assert.eq(
        0, newVersion.i, "The shard version should have reset, but the minor value is not zero");

    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failApplyChunkOps', mode: 'off'}));
    assert.commandWorked(st.shard0.getDB("admin").runCommand(
        {configureFailPoint: 'failCommitMigrationCommand', mode: 'off'}));

    st.stop();

})();
