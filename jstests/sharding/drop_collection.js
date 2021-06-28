/**
 * Basic test from the drop collection command on a sharded cluster that verifies collections are
 * cleaned up properly.
 */
(function() {
"use strict";

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

    // No more chunks
    const errMsg = "Found collection entry in 'config.collection' after drop.";
    // Before 5.0 chunks were indexed by ns, now by uuid
    assert.eq(0, configDB.chunks.countDocuments({ns: ns}), errMsg);
    if (uuid != null) {
        assert.eq(0, configDB.chunks.countDocuments({uuid: uuid}), errMsg);
    }

    // No more coll entry
    assert.eq(null, st.s.getCollection(ns).exists());

    // Check for the collection with majority RC to verify that the write to remove the collection
    // document from the catalog has propagated to the majority snapshot. Note that here we
    // explicitly use a command instead of going through the driver's 'find' helper, in order to be
    // able to specify a 'majority' read concern.
    //
    // assert.eq(0, configDB.chunks.countDocuments({_id: ns{));
    //
    // TODO (SERVER-51881): Remove this check after 5.0 is released
    var collEntry =
        assert
            .commandWorked(configDB.runCommand(
                {find: 'collections', filter: {_id: ns}, readConcern: {'level': 'majority'}}))
            .cursor.firstBatch;
    if (collEntry.length > 0) {
        assert.eq(1, collEntry.length);
        assert.eq(true, collEntry[0].dropped);
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
    const coll = db['unshardedColl0'];
    // Create the database
    assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
    for (var i = 0; i < 3; i++) {
        // Create the collection
        assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
        assert.commandWorked(coll.insert({x: 123}));
        assert.eq(1, coll.countDocuments({x: 123}));
        // Drop the collection
        assert.commandWorked(db.runCommand({drop: coll.getName()}));
        assertCollectionDropped(coll.getFullName());
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
    coll.drop();

    jsTest.log("Re-Create sharded collection with one chunk on shard 0");
    st.shardColl(coll, {skey: 1}, false, false);
    assert.eq(1,
              findChunksUtil.countChunksForNs(
                  configDB, coll.getFullName(), {shard: st.shard0.shardName}));

    jsTest.log("Move primary of DB to shard 1");
    st.ensurePrimaryShard(db.getName(), st.shard1.shardName);

    jsTest.log("Drop sharded collection");
    coll.drop();
}

jsTest.log(
    "Test that dropping a non-sharded collection, relevant events are properly logged on CSRS");
{
    // Create a non-sharded collection
    const db = getNewDb();
    const coll = db['unshardedColl'];
    assert.commandWorked(coll.insert({x: 1}));

    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    // Verify that the drop collection start event has been logged
    const startLogCount =
        configDB.changelog.countDocuments({what: 'dropCollection.start', ns: coll.getFullName()});
    assert.gte(1, startLogCount, "dropCollection start event not found in changelog");

    // Verify that the drop collection end event has been logged
    const endLogCount =
        configDB.changelog.countDocuments({what: 'dropCollection', ms: coll.getFullName()});
    assert.gte(1, endLogCount, "dropCollection end event not found in changelog");
}

jsTest.log("Test that dropping a sharded collection, relevant events are properly logged on CSRS");
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
    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    // Verify that the drop collection start event has been logged
    const startLogCount =
        configDB.changelog.countDocuments({what: 'dropCollection.start', ns: coll.getFullName()});
    assert.gte(1, startLogCount, "dropCollection start event not found in changelog");

    // Verify that the drop collection end event has been logged
    const endLogCount =
        configDB.changelog.countDocuments({what: 'dropCollection', ms: coll.getFullName()});
    assert.gte(1, endLogCount, "dropCollection end event not found in changelog");
}

jsTest.log("Test that dropping a sharded collection, the cached metadata on shards is cleaned up");
{
    // Create a sharded collection
    const db = getNewDb();
    const coll = db['shardedColl'];
    assert.commandWorked(
        st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

    // Distribute the chunks among the shards
    assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(coll.insert({_id: 10}));
    assert.commandWorked(coll.insert({_id: -10}));

    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));

    // Verify that the cached metadata on shards is cleaned up
    for (let configDb of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
        assert.eq(configDb['cache.collections'].countDocuments({_id: coll.getFullName()}), 0);
        assert(!configDb['cache.chunks.' + coll.getFullName()].exists());
    }
}

st.stop();
})();
