/**
 * Tests catalog shard topology.
 *
 * @tags: [
 *   requires_fcv_63,
 *   featureFlagCatalogShard,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const kCatalogShardId = "catalogShard";
const dbName = "foo";
const collName = "bar";
const ns = dbName + "." + collName;

function basicCRUD(conn) {
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
    // Remove the catalog shard.
    //

    let removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: kCatalogShardId}));
    assert.eq("started", removeRes.state);

    // The removal won't complete until all chunks and dbs are moved off the catalog shard.
    removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: kCatalogShardId}));
    assert.eq("ongoing", removeRes.state);

    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: -1}, to: newShardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: "config.system.sessions", find: {_id: 0}, to: newShardName}));

    // Still blocked until the db has been moved away.
    removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: kCatalogShardId}));
    assert.eq("ongoing", removeRes.state);

    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: newShardName}));

    removeRes = assert.commandWorked(st.s0.adminCommand({removeShard: kCatalogShardId}));
    assert.eq("completed", removeRes.state);

    // Basic CRUD and sharded DDL work.
    basicCRUD(st.s);
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 220}}));
    basicCRUD(st.s);

    // Flushing routing / db cache updates works.
    flushRoutingAndDBCacheUpdates(st.configRS.getPrimary());
}

{
    //
    // Add back the catalog shard.
    //

    // movePrimary won't delete from the source, so drop the moved db directly to avoid a conflict
    // in addShard.
    assert.commandWorked(st.configRS.getPrimary().getDB(dbName).dropDatabase());
    assert.commandWorked(st.s.adminCommand({transitionToCatalogShard: 1}));

    // Basic CRUD and sharded DDL work.
    basicCRUD(st.s);
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 0}, to: configShardName}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 5}}));
    basicCRUD(st.s);
}

st.stop();
newShardRS.stopSet();
}());
