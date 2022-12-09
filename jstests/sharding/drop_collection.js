/**
 * Basic test from the drop collection command on a sharded cluster that verifies collections are
 * cleaned up properly.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({shards: 2});

const configDB = st.s.getDB('config');
const dbName = 'testDropCollDB';
var dbCounter = 0;

function getCollectionUUID(ns) {
    return configDB.collections.findOne({_id: ns}).uuid;
}

function getNewDb() {
    return st.s.getDB(dbName + dbCounter++);
}

function assertCollectionDropped(ns, uuid = null) {
    // No more documents
    assert.eq(
        0, st.s.getCollection(ns).countDocuments({}), "Found documents for dropped collection.");

    // No more tags
    assert.eq(0,
              configDB.tags.countDocuments({ns: ns}),
              "Found unexpected tag for a collection after drop.");

    // Sharding metadata checks

    // No more coll entry
    assert.eq(null, st.s.getCollection(ns).exists());
    assert.eq(0,
              configDB.collections.countDocuments({_id: ns}),
              "Found collection entry in 'config.collection' after drop.");

    // No more chunks
    if (uuid != null) {
        assert.eq(0,
                  configDB.chunks.countDocuments({uuid: uuid}),
                  "Found references to collection uuid in 'config.chunks' after drop.");
    }

    // Verify that persisted cached metadata was removed as part of the dropCollection
    const chunksCollName = 'cache.chunks.' + ns;
    for (let configDb of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
        assert.eq(configDb['cache.collections'].countDocuments({_id: ns}),
                  0,
                  "Found collection entry in 'config.cache.collections' after drop.");
        assert(!configDb[chunksCollName].exists());
    }
}

jsTest.log("Drop unsharded collection.");
{
    const db = getNewDb();
    const coll = db['unshardedColl0'];
    // Create the collection
    assert.commandWorked(coll.insert({x: 1}));
    assert.eq(1, coll.countDocuments({x: 1}));
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName());
}

jsTest.log("Drop unsharded collection also remove tags.");
{
    const db = getNewDb();
    const coll = db['unshardedColl1'];
    // Create the database
    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
    // Add a zone
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'zone1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: coll.getFullName(), min: {x: 0}, max: {x: 10}, zone: 'zone1'}));
    assert.eq(1, configDB.tags.countDocuments({ns: coll.getFullName()}));
    // Create the collection
    assert.commandWorked(coll.insert({x: 1}));
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName());
}

jsTest.log("Drop sharded collection repeated.");
{
    const db = getNewDb();
    const coll = db['shardedColl0'];
    // Create the database
    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
    for (var i = 0; i < 3; i++) {
        // Create the collection
        assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
        assert.commandWorked(coll.insert({x: 123}));
        assert.eq(1, coll.countDocuments({x: 123}));

        // Drop the collection
        var uuid = getCollectionUUID(coll.getFullName());
        assert.commandWorked(db.runCommand({drop: coll.getName()}));
        assertCollectionDropped(coll.getFullName(), uuid);
    }
}

jsTest.log("Drop unexistent collections also remove tags.");
{
    const db = getNewDb();
    const coll = db['unexistent'];
    // Create the database
    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
    // Add a zone
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'zone1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: coll.getFullName(), min: {x: -1}, max: {x: 1}, zone: 'zone1'}));
    assert.eq(1, configDB.tags.countDocuments({ns: coll.getFullName()}));
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName());
}

jsTest.log("Drop a sharded collection.");
{
    const db = getNewDb();
    const coll = db['shardedColl1'];

    assert.commandWorked(
        st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    // Spread chunks on all the shards
    assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));
    // Insert two documents
    assert.commandWorked(coll.insert({_id: 10}));
    assert.commandWorked(coll.insert({_id: -10}));

    // Check that data is in place
    assert.eq(2, coll.countDocuments({}));
    assert.eq(1, configDB.collections.countDocuments({_id: coll.getFullName()}));

    // Drop the collection
    const uuid = getCollectionUUID(coll.getFullName());
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName(), uuid);

    // Call drop again to verify that the command is idempotent.
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
}

jsTest.log("Drop a sharded collection with zones.");
{
    const db = getNewDb();
    const coll = db['shardedColl2'];

    assert.commandWorked(
        st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    // Spread chunks on all the shards
    assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));
    // Add tags
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: 'foo'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: coll.getFullName(), min: {_id: 0}, max: {_id: 10}, zone: 'foo'}));

    assert.commandWorked(coll.insert({_id: -10}));
    assert.commandWorked(coll.insert({_id: 10}));

    // Checks that data and metadata are in place
    assert.eq(1, configDB.tags.countDocuments({ns: coll.getFullName()}));
    assert.eq(2, coll.countDocuments({}));
    assert.eq(2, findChunksUtil.countChunksForNs(configDB, coll.getFullName()));
    assert.neq(null, st.shard0.getCollection(coll.getFullName()).findOne({_id: -10}));
    assert.neq(null, st.shard1.getCollection(coll.getFullName()).findOne({_id: 10}));

    // Drop the collection
    const uuid = getCollectionUUID(coll.getFullName());
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName(), uuid);

    // Call drop again to verify that the command is idempotent.
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
}

jsTest.log("Move primary with drop and recreate - new primary no chunks.");
/*
 * Test that moving database primary works after dropping and recreating the same sharded
 * collection.
 * The new primary never owned a chunk of the sharded collection.
 */
{
    const db = getNewDb();
    const coll = db['movePrimaryNoChunks'];

    jsTest.log("Create sharded collection with on chunk on shad 0");
    assert.commandWorked(
        st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    st.shardColl(coll, {skey: 1}, false, false);

    jsTest.log("Move database primary back and forth shard 1");
    st.ensurePrimaryShard(db.getName(), st.shard1.shardName);
    st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

    jsTest.log("Drop sharded collection");
    var uuid = getCollectionUUID(coll.getFullName());
    coll.drop();
    assertCollectionDropped(coll.getFullName(), uuid);

    jsTest.log("Re-Create sharded collection on shard 0");
    st.shardColl(coll, {skey: 1}, false, false);

    jsTest.log("Move database primary to shard 1");
    st.ensurePrimaryShard(db.getName(), st.shard1.shardName);

    jsTest.log("Drop sharded collection");
    uuid = getCollectionUUID(coll.getFullName());
    coll.drop();
    assertCollectionDropped(coll.getFullName(), uuid);
}

jsTest.log("Move primary with drop and recreate - new primary own chunks.");
/*
 * Test that moving database primary works after dropping and recreating the same sharded
 * collection.
 * The new primary previously owned a chunk of the original collection.
 */
{
    const db = getNewDb();
    const coll = db['movePrimaryWithChunks'];

    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));

    jsTest.log("Create sharded collection with two chunks on each shard");
    st.ensurePrimaryShard(db.getName(), st.shard0.shardName);
    st.shardColl(coll, {skey: 1}, {skey: 0}, {skey: 0});

    assert.eq(1,
              findChunksUtil.countChunksForNs(
                  configDB, coll.getFullName(), {shard: st.shard0.shardName}));
    assert.eq(1,
              findChunksUtil.countChunksForNs(
                  configDB, coll.getFullName(), {shard: st.shard1.shardName}));
    jsTest.log("Move all chunks to shard 0");
    assert.commandWorked(st.s.adminCommand({
        moveChunk: coll.getFullName(),
        find: {skey: 10},
        to: st.shard0.shardName,
        _waitForDelete: true
    }));
    assert.eq(2,
              findChunksUtil.countChunksForNs(
                  configDB, coll.getFullName(), {shard: st.shard0.shardName}));
    assert.eq(0,
              findChunksUtil.countChunksForNs(
                  configDB, coll.getFullName(), {shard: st.shard1.shardName}));

    jsTest.log("Drop sharded collection");
    var uuid = getCollectionUUID(coll.getFullName());
    coll.drop();
    assertCollectionDropped(coll.getFullName(), uuid);

    jsTest.log("Re-Create sharded collection with one chunk on shard 0");
    st.shardColl(coll, {skey: 1}, false, false);
    assert.eq(1,
              findChunksUtil.countChunksForNs(
                  configDB, coll.getFullName(), {shard: st.shard0.shardName}));

    jsTest.log("Move primary of DB to shard 1");
    st.ensurePrimaryShard(db.getName(), st.shard1.shardName);

    jsTest.log("Drop sharded collection");
    uuid = getCollectionUUID(coll.getFullName());
    coll.drop();
    assertCollectionDropped(coll.getFullName(), uuid);
}

jsTest.log("Test that dropping an unsharded collection, relevant events are logged on CSRS.");
{
    // Create a non-sharded collection
    const db = getNewDb();
    const coll = db['unshardedColl'];
    assert.commandWorked(coll.insert({x: 1}));

    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName());

    // Verify that the drop collection start event has been logged
    const startLogCount =
        configDB.changelog.countDocuments({what: 'dropCollection.start', ns: coll.getFullName()});
    assert.gte(1, startLogCount, "dropCollection start event not found in changelog");

    // Verify that the drop collection end event has been logged
    const endLogCount =
        configDB.changelog.countDocuments({what: 'dropCollection', ms: coll.getFullName()});
    assert.gte(1, endLogCount, "dropCollection end event not found in changelog");
}

jsTest.log("Test that dropping a sharded collection, relevant events are logged on CSRS.");
{
    // Create a sharded collection
    const db = getNewDb();
    const coll = db['shardedColl'];
    assert.commandWorked(
        st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

    // Distribute the chunks among the shards
    assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));
    assert.commandWorked(coll.insert({_id: 10}));
    assert.commandWorked(coll.insert({_id: -10}));

    // Drop the collection
    const uuid = getCollectionUUID(coll.getFullName());
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName(), uuid);

    // Verify that the drop collection start event has been logged
    const startLogCount =
        configDB.changelog.countDocuments({what: 'dropCollection.start', ns: coll.getFullName()});
    assert.gte(1, startLogCount, "dropCollection start event not found in changelog");

    // Verify that the drop collection end event has been logged
    const endLogCount =
        configDB.changelog.countDocuments({what: 'dropCollection', ms: coll.getFullName()});
    assert.gte(1, endLogCount, "dropCollection end event not found in changelog");
}

jsTest.log("Test that dropping a sharded collection, the cached metadata on shards is cleaned up.");
{
    // Create a sharded collection
    const db = getNewDb();
    const coll = db['shardedColl'];
    assert.commandWorked(
        st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    assert.commandWorked(coll.insert({_id: 10}));

    // At this point only one shard has valid filtering information (i.e. the one that has data).
    // Below we are forcing to all shards to have their valid filtering information. For the shard
    // that doesn't own any data, that means having a filtering information stating that no chunks
    // are owned by that shard.
    assert.commandWorked(
        st.shard0.adminCommand({_flushRoutingTableCacheUpdates: coll.getFullName()}));
    assert.commandWorked(
        st.shard1.adminCommand({_flushRoutingTableCacheUpdates: coll.getFullName()}));

    // Drop the collection
    const uuid = getCollectionUUID(coll.getFullName());
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName(), uuid);
}

jsTest.log("Test that dropping a sharded collection, the range deletion documents are deleted.");
{
    const db = getNewDb();

    // Requires all primary shard nodes to be running the latest version
    let latestVersion = true;
    st.rs0.nodes.forEach(function(node) {
        const fcvDoc = node.getDB(db.getName())
                           .adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, '6.2') < 0) {
            latestVersion = false;
        }
    });

    if (latestVersion) {
        jsTest.log("All primary shard nodes are running the latest version");

        // Create a sharded collection
        const coll = db['shardedColl'];
        assert.commandWorked(
            st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        const collUUID = st.config.collections.findOne({_id: coll.getFullName()}).uuid;

        // Pause before first range deletion task
        let suspendRangeDeletion = configureFailPoint(st.shard0, "suspendRangeDeletion");

        // Distribute the chunks among the shards
        assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: -10}}));
        assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 10}}));
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: coll.getFullName(), find: {_id: -10}, to: st.shard1.shardName}));
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: coll.getFullName(), find: {_id: 10}, to: st.shard1.shardName}));

        assert.eq(2, st.shard0.getDB("config").rangeDeletions.count({collectionUuid: collUUID}));

        // Drop the collection
        const uuid = getCollectionUUID(coll.getFullName());
        assert.commandWorked(db.runCommand({drop: coll.getName()}));
        assertCollectionDropped(coll.getFullName(), uuid);

        // Verify that the range deletion documents are deleted
        assert.eq(0, st.shard0.getDB("config").rangeDeletions.count({collectionUuid: collUUID}));

        // Allow everything to finish
        suspendRangeDeletion.off();
    }
}

st.stop();
})();
