/*
 * Tests to validate the correct behaviour of checkMetadataConsistency command.
 *
 * TODO SERVER-74445: Fix cluster level checkMetadataConsistency command with a catalog shard.
 * @tags: [featureFlagCheckMetadataConsistency, temporary_catalog_shard_incompatible]
 */

(function() {
'use strict';

// Configure initial sharding cluster
const st = new ShardingTest({});
const mongos = st.s;

const dbName = "testCheckMetadataConsistencyDB";
var dbCounter = 0;

function getNewDb() {
    return mongos.getDB(dbName + dbCounter++);
}

(function testNotImplementedLevelModes() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll.getFullName(), key: {_id: 1}}));

    // Collection level mode command
    assert.commandFailedWithCode(db.runCommand({checkMetadataConsistency: "coll"}),
                                 ErrorCodes.NotImplemented);

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
})();

(function testCursor() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll1.insert({_id: 'foo'}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll2.insert({_id: 'foo'}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll3.insert({_id: 'foo'}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll4.insert({_id: 'foo'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll1.getFullName(), key: {_id: 1}}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll2.getFullName(), key: {_id: 1}}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll3.getFullName(), key: {_id: 1}}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll4.getFullName(), key: {_id: 1}}));

    // Check correct behaviour of cursor with DBCommandCursor
    let res = db.runCommand({checkMetadataConsistency: 1, cursor: {batchSize: 1}});
    assert.commandWorked(res);

    assert.eq(1, res.cursor.firstBatch.length);
    const cursor = new DBCommandCursor(db, res);
    for (let i = 0; i < 4; i++) {
        assert(cursor.hasNext());
        const inconsistency = cursor.next();
        assert.eq(inconsistency.type, "CollectionUUIDMismatch");
    }
    assert(!cursor.hasNext());

    // Check correct behaviour of cursor with GetMore
    res = db.runCommand({checkMetadataConsistency: 1, cursor: {batchSize: 3}});
    assert.commandWorked(res);
    assert.eq(3, res.cursor.firstBatch.length);

    const getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf(".") + 1);
    res =
        assert.commandWorked(db.runCommand({getMore: res.cursor.id, collection: getMoreCollName}));
    assert.eq(1, res.cursor.nextBatch.length);

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    res = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, res.length, tojson(res));
})();

(function testCollectionUUIDMismatchInconsistency() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll.getFullName(), key: {_id: 1}}));

    // Database level mode command
    let inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("CollectionUUIDMismatch", inconsistencies[0].type, tojson(inconsistencies[0]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
})();

(function testMisplacedCollection() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    // Database level mode command
    let inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("MisplacedCollection", inconsistencies[0].type, tojson(inconsistencies[0]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
})();

(function testMissingShardKeyInconsistency() {
    const db = getNewDb();
    const kSourceCollName = "coll";

    st.shardColl(
        kSourceCollName, {skey: 1}, {skey: 0}, {skey: 1}, db.getName(), true /* waitForDelete */);

    // Connect directly to shards to bypass the mongos checks for dropping shard key indexes
    assert.commandWorked(st.shard0.getDB(db.getName()).coll.dropIndex({skey: 1}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll.dropIndex({skey: 1}));

    assert.commandWorked(st.s.getDB(db.getName()).coll.insert({skey: -10}));
    assert.commandWorked(st.s.getDB(db.getName()).coll.insert({skey: 10}));

    // Database level mode command
    let inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(2, inconsistencies.length, tojson(inconsistencies));
    assert.eq("MissingShardKeyIndex", inconsistencies[0].type, tojson(inconsistencies[0]));
    assert.eq("MissingShardKeyIndex", inconsistencies[1].type, tojson(inconsistencies[1]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
})();

(function testHiddenShardedCollections() {
    const kSourceCollName = "coll";
    const db1 = getNewDb();
    const coll1 = db1[kSourceCollName];
    const db2 = getNewDb();
    const coll2 = db2[kSourceCollName];

    // Create two sharded collections in two different databases
    st.shardColl(coll1, {skey: 1});
    st.shardColl(coll2, {skey: 1});

    // Save db1 and db2 configuration to restore it later
    const configDatabasesColl = mongos.getDB('config').databases;
    const db1ConfigEntry = configDatabasesColl.findOne({_id: db1.getName()});
    const db2ConfigEntry = configDatabasesColl.findOne({_id: db2.getName()});

    // Check that there are no inconsistencies so far
    let inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));

    // Remove db1 so that coll1 became hidden
    assert.commandWorked(configDatabasesColl.deleteOne({_id: db1.getName()}));

    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length, tojson(inconsistencies));
    assert.eq("HiddenShardedCollection", inconsistencies[0].type, tojson(inconsistencies[0]));
    assert.eq(coll1.getFullName(), inconsistencies[0].details.ns, tojson(inconsistencies[0]));

    // Remove db2 so that coll2 also became hidden
    assert.commandWorked(configDatabasesColl.deleteOne({_id: db2.getName()}));

    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(2, inconsistencies.length, tojson(inconsistencies));
    assert.eq("HiddenShardedCollection", inconsistencies[0].type, tojson(inconsistencies[0]));
    assert.eq(coll1.getFullName(), inconsistencies[0].details.ns, tojson(inconsistencies[0]));
    assert.eq("HiddenShardedCollection", inconsistencies[1].type, tojson(inconsistencies[1]));
    assert.eq(coll2.getFullName(), inconsistencies[1].details.ns, tojson(inconsistencies[1]));

    // Restore db1 and db2 configuration to ensure the correct behavior of dropDatabase operations
    assert.commandWorked(configDatabasesColl.insertMany([db1ConfigEntry, db2ConfigEntry]));

    // Clean up the database to pass the hooks that detect inconsistencies
    db1.dropDatabase();
    db2.dropDatabase();
    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
})();

(function testClusterLevelMode() {
    const db_MisplacedCollection1 = getNewDb();
    const db_MisplacedCollection2 = getNewDb();
    const db_CollectionUUIDMismatch = getNewDb();

    // Insert MisplacedCollection inconsistency in db_MisplacedCollection1
    assert.commandWorked(mongos.adminCommand(
        {enableSharding: db_MisplacedCollection1.getName(), primaryShard: st.shard0.shardName}));
    assert.commandWorked(
        st.shard1.getDB(db_MisplacedCollection1.getName()).coll.insert({_id: 'foo'}));

    // Insert MisplacedCollection inconsistency in db_MisplacedCollection2
    assert.commandWorked(mongos.adminCommand(
        {enableSharding: db_MisplacedCollection2.getName(), primaryShard: st.shard1.shardName}));
    assert.commandWorked(
        st.shard0.getDB(db_MisplacedCollection2.getName()).coll.insert({_id: 'foo'}));

    // Insert CollectionUUIDMismatch inconsistency in db_CollectionUUIDMismatch
    assert.commandWorked(mongos.adminCommand(
        {enableSharding: db_CollectionUUIDMismatch.getName(), primaryShard: st.shard1.shardName}));

    assert.commandWorked(
        st.shard0.getDB(db_CollectionUUIDMismatch.getName()).coll.insert({_id: 'foo'}));

    assert.commandWorked(st.s.adminCommand(
        {shardCollection: db_CollectionUUIDMismatch.coll.getFullName(), key: {_id: 1}}));

    // Cluster level mode command
    let inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();

    // Check that there are 3 inconsistencies: 2 MisplacedCollection and 1 CollectionUUIDMismatch
    assert.eq(3, inconsistencies.length, tojson(inconsistencies));
    const count = inconsistencies.reduce((acc, object) => {
        return object.type === "MisplacedCollection" ? acc + 1 : acc;
    }, 0);
    assert.eq(2, count, tojson(inconsistencies));
    assert(inconsistencies.some(object => object.type === "CollectionUUIDMismatch"),
           tojson(inconsistencies));

    // Clean up the databases to pass the hooks that detect inconsistencies
    db_MisplacedCollection1.dropDatabase();
    db_MisplacedCollection2.dropDatabase();
    db_CollectionUUIDMismatch.dropDatabase();
    inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));
})();

st.stop();
})();
