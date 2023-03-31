/**
 * Tests catalog shard topology.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_fcv_70,
 *   featureFlagCatalogShard,
 *   featureFlagTransitionToCatalogShard,
 * ]
 */
(function() {
"use strict";

// TODO (SERVER-74534): Enable the metadata consistency check when it will work with co-located
// configsvr.
TestData.skipCheckMetadataConsistency = true;

load("jstests/libs/fail_point_util.js");
load("jstests/libs/write_concern_util.js");

const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;
const unshardedDbName = "unsharded_db";
const unshardedNs = unshardedDbName + ".unsharded_coll";
const indexedNs = "db_with_index.coll";

function basicCRUD(conn) {
    assert.commandWorked(st.s.getCollection(unshardedNs).insert([{x: 1}, {x: -1}]));

    assert.commandWorked(conn.getCollection(ns).insert({_id: 1, x: 1}));
    assert.sameMembers(conn.getCollection(ns).find({x: 1}).toArray(), [{_id: 1, x: 1}]);
    assert.commandWorked(conn.getCollection(ns).remove({x: 1}));
    assert.eq(conn.getCollection(ns).find({x: 1}).toArray().length, 0);
}

function flushRoutingAndDBCacheUpdates(conn) {
    assert.commandWorked(conn.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(conn.adminCommand({_flushDatabaseCacheUpdates: dbName}));
    assert.commandWorked(conn.adminCommand({_flushRoutingTableCacheUpdates: "does.not.exist"}));
    assert.commandWorked(conn.adminCommand({_flushDatabaseCacheUpdates: "notRealDB"}));
}

const st = new ShardingTest({
    shards: 1,
    config: 3,
    catalogShard: true,
});

const configShardName = st.shard0.shardName;

{
    //
    // Basic unsharded CRUD.
    //
    basicCRUD(st.s);
}

{
    //
    // Basic sharded CRUD.
    //
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: 1}}));

    basicCRUD(st.s);

    // Flushing routing / db cache updates works.
    flushRoutingAndDBCacheUpdates(st.configRS.getPrimary());
}

// Add a shard to move chunks to and from it in later tests.
const newShardRS = new ReplSetTest({name: "new-shard-rs", nodes: 1});
newShardRS.startSet({shardsvr: ""});
newShardRS.initiate();
const newShardName =
    assert.commandWorked(st.s.adminCommand({addShard: newShardRS.getURL()})).shardAdded;

{
    //
    // Basic sharded DDL.
    //
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 0}}));
    assert.commandWorked(st.s.getCollection(ns).insert([{skey: 1}, {skey: -1}]));

    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: newShardName}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: configShardName}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: newShardName}));
}

{
    //
    // Basic secondary reads.
    //
    assert.commandWorked(
        st.s.getCollection(ns).insert({readFromSecondary: 1, skey: -1}, {writeConcern: {w: 3}}));
    let secondaryRes = assert.commandWorked(st.s.getDB(dbName).runCommand({
        find: collName,
        filter: {readFromSecondary: 1, skey: -1},
        $readPreference: {mode: "secondary"}
    }));
    assert.eq(secondaryRes.cursor.firstBatch.length, 1, tojson(secondaryRes));
}

{
    //
    // Failover in shard role works.
    //
    st.configRS.stepUp(st.configRS.getSecondary());

    // Basic CRUD and sharded DDL still works.
    basicCRUD(st.s);
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 20}}));
}

{
    //
    // Restart in shard role works. Restart all nodes to verify they don't rely on a majority of
    // nodes being up.
    //
    const configNodes = st.configRS.nodes;
    configNodes.forEach(node => {
        st.configRS.restart(node, undefined, undefined, false /* wait */);
    });
    st.configRS.getPrimary();  // Waits for a stable primary.

    // Basic CRUD and sharded DDL still works.
    basicCRUD(st.s);
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 40}}));
}

{
    //
    // ShardingStateRecovery doesn't block step up.
    //

    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: configShardName}));

    const hangMigrationFp = configureFailPoint(st.configRS.getPrimary(), "moveChunkHangAtStep5");
    const moveChunkThread = new Thread(function(mongosHost, ns, newShardName) {
        const mongos = new Mongo(mongosHost);
        assert.commandWorked(
            mongos.adminCommand({moveChunk: ns, find: {skey: 0}, to: newShardName}));
    }, st.s.host, ns, newShardName);
    moveChunkThread.start();
    hangMigrationFp.wait();

    // Stepping up shouldn't hang because of ShardingStateRecovery.
    st.configRS.stepUp(st.configRS.getSecondary());

    hangMigrationFp.off();
    moveChunkThread.join();
}

{
    //
    // Collections on the config server support changeStreamPreAndPostImages when the config server
    // is acting as a shard.
    //
    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {collMod: collName, changeStreamPreAndPostImages: {enabled: true}}));
    basicCRUD(st.s);

    // Sharding metadata collections still cannot have changeStreamPreAndPostImages set.
    assert.commandFailedWithCode(
        st.s.getDB("config").runCommand(
            {collMod: "chunks", changeStreamPreAndPostImages: {enabled: true}}),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        st.s.getDB("admin").runCommand(
            {collMod: "system.version", changeStreamPreAndPostImages: {enabled: true}}),
        ErrorCodes.InvalidOptions);
}

{
    //
    // Can't remove catalogShard using the removeShard command.
    //

    assert.commandFailedWithCode(st.s.adminCommand({removeShard: "config"}),
                                 ErrorCodes.IllegalOperation);
}

{
    //
    // Remove the catalog shard.
    //
    let configPrimary = st.configRS.getPrimary();

    // Shard a second collection to verify it gets dropped locally by the transition.
    assert.commandWorked(st.s.adminCommand({shardCollection: indexedNs, key: {_id: 1}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: indexedNs, find: {_id: 0}, to: configShardName}));
    assert.commandWorked(st.s.getCollection(indexedNs).createIndex({oldKey: 1}));

    // Use write concern to verify the commands support them. Any values weaker than the default
    // sharding metadata write concerns will be upgraded.
    let removeRes = assert.commandWorked(
        st.s0.adminCommand({transitionToDedicatedConfigServer: 1, writeConcern: {wtimeout: 100}}));
    assert.eq("started", removeRes.state);

    // The removal won't complete until all chunks and dbs are moved off the catalog shard.
    removeRes = assert.commandWorked(st.s0.adminCommand({transitionToDedicatedConfigServer: 1}));
    assert.eq("ongoing", removeRes.state);

    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: -1}, to: newShardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: indexedNs, find: {_id: 0}, to: newShardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: "config.system.sessions", find: {_id: 0}, to: newShardName}));

    // Still blocked until the db has been moved away.
    removeRes = assert.commandWorked(st.s0.adminCommand({transitionToDedicatedConfigServer: 1}));
    assert.eq("ongoing", removeRes.state);

    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: newShardName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: unshardedDbName, to: newShardName}));

    // The draining sharded collections should not have been locally dropped yet.
    assert(configPrimary.getCollection(ns).exists());
    assert(configPrimary.getCollection(indexedNs).exists());
    assert.sameMembers(configPrimary.getCollection(indexedNs).getIndexKeys(),
                       [{_id: 1}, {oldKey: 1}]);
    assert(configPrimary.getCollection("config.system.sessions").exists());

    // Start the final transition command. This will trigger locally dropping collections on the
    // config server. Hang after removing one collection and trigger a failover to verify the final
    // transition can be resumed on the new primary and the collection dropping is idempotent.
    const hangRemoveFp = configureFailPoint(
        st.configRS.getPrimary(), "hangAfterDroppingCollectionInTransitionToDedicatedConfigServer");
    const finishRemoveThread = new Thread(function(mongosHost) {
        const mongos = new Mongo(mongosHost);
        return mongos.adminCommand({transitionToDedicatedConfigServer: 1});
    }, st.s.host);
    finishRemoveThread.start();

    hangRemoveFp.wait();
    st.configRS.stepUp(st.configRS.getSecondary());
    hangRemoveFp.off();
    configPrimary = st.configRS.getPrimary();

    finishRemoveThread.join();
    removeRes = assert.commandWorked(finishRemoveThread.returnData());
    assert.eq("completed", removeRes.state);

    // All sharded collections should have been dropped locally from the config server.
    assert(!configPrimary.getCollection(ns).exists());
    assert(!configPrimary.getCollection(indexedNs).exists());
    assert.sameMembers(configPrimary.getCollection(indexedNs).getIndexKeys(), []);
    assert(!configPrimary.getCollection("config.system.sessions").exists());

    // Basic CRUD and sharded DDL work.
    basicCRUD(st.s);
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 220}}));
    basicCRUD(st.s);

    // Flushing routing / db cache updates works.
    flushRoutingAndDBCacheUpdates(configPrimary);

    //
    // A config server that isn't currently a shard can support changeStreamPreAndPostImages. Note
    // sharding metadata databases cannot have this option, so we have to make a direct connection
    // to the config server to create a collection on a different db.
    //
    const directConfigNS = "directDB.onConfig";
    assert.commandWorked(configPrimary.getCollection(directConfigNS).insert({x: 1}));
    assert.commandWorked(configPrimary.getDB("directDB").runCommand({
        collMod: "onConfig",
        changeStreamPreAndPostImages: {enabled: true}
    }));
}

{
    //
    // Can't create catalogShard using the addShard command.
    //

    assert.commandFailed(st.s.adminCommand({addShard: st.configRS.getURL(), name: "config"}));

    // Ensure that the config server's RSM hasn't been removed and that it can still target itself.
    st.s.getDB("config")
        .chunks
        .aggregate([
            {$lookup: {from: "shards", localField: "shard", foreignField: "_id", as: "shardHost"}},
            {$unwind: "$shardHost"},
        ])
        .toArray();
}

{
    //
    // Add back the catalog shard.
    //

    // Create an index while the collection is not on the config server to verify it clones the
    // correct indexes when receiving its first chunk after the transition.
    assert.commandWorked(st.s.getCollection(indexedNs).createIndex({newKey: 1}));

    // Use write concern to verify the command support them. Any values weaker than the default
    // sharding metadata write concerns will be upgraded.
    assert.commandWorked(
        st.s.adminCommand({transitionToCatalogShard: 1, writeConcern: {wtimeout: 100}}));

    // Basic CRUD and sharded DDL work.
    basicCRUD(st.s);
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: configShardName}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 5}}));
    basicCRUD(st.s);

    // Move a chunk for the indexed collection to the config server and it should create the correct
    // index locally.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: indexedNs, find: {_id: 0}, to: configShardName}));
    assert.sameMembers(st.configRS.getPrimary().getCollection(indexedNs).getIndexKeys(),
                       [{_id: 1}, {oldKey: 1}, {newKey: 1}]);
}

{
    //
    // transitionToCatalogShard requires replication to all config server nodes.
    //
    // TODO SERVER-75391: Remove.
    //

    // Transition to dedicated mode so the config server can transition back to catalog shard mode.
    let removeRes = assert.commandWorked(st.s.adminCommand({transitionToDedicatedConfigServer: 1}));
    assert.eq("started", removeRes.state);
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: newShardName}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 5}, to: newShardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: indexedNs, find: {_id: 0}, to: newShardName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: "directDB", to: newShardName}));
    assert.commandWorked(st.s.adminCommand({transitionToDedicatedConfigServer: 1}));

    // transitionToCatalogShard times out with a lagged config secondary despite having a majority
    // of its set still replicating.
    const laggedSecondary = st.configRS.getSecondary();
    st.configRS.awaitReplication();
    stopServerReplication(laggedSecondary);
    assert.commandFailedWithCode(st.s.adminCommand({transitionToCatalogShard: 1, maxTimeMS: 1000}),
                                 ErrorCodes.MaxTimeMSExpired);
    restartServerReplication(laggedSecondary);

    // Now it succeeds.
    assert.commandWorked(st.s.adminCommand({transitionToCatalogShard: 1}));
}

st.stop();
newShardRS.stopSet();
}());
