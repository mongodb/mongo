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
        assert.eq(inconsistency.type, "UUIDMismatch");
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
})();

(function testUUIDMismatchInconsistency() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll.getFullName(), key: {_id: 1}}));

    // Database level mode command
    const inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length);
    assert.eq("UUIDMismatch", inconsistencies[0].type);

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
})();

(function testHiddenUnshardedCollection() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    // Database level mode command
    const inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(1, inconsistencies.length);
    assert.eq("HiddenUnshardedCollection", inconsistencies[0].type);

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
})();

(function testMissingShardKeyInconsistency() {
    const db = getNewDb();
    const kSourceCollName = "coll";

    st.shardColl(
        kSourceCollName, {skey: 1}, {skey: 0}, {skey: 1}, db.getName(), true /* waitForDelete */);

    // Connect directly to shards to bypass the mongos checks for dropping shard key indexes
    assert.commandWorked(st.shard0.getDB(db.getName()).coll.dropIndex({skey: 1}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll.dropIndex({skey: 1}));

    // Database level mode command
    const inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(2, inconsistencies.length);
    assert.eq("MissingShardKeyIndex", inconsistencies[0].type);
    assert.eq("MissingShardKeyIndex", inconsistencies[1].type);

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
})();

(function testLastShardKeyIndexMultiKeyInconsistency() {
    const db = getNewDb();
    const kSourceCollName = "coll";

    // Create a multikey index compatible with the shard key
    assert.commandWorked(mongos.getDB(db.getName()).coll.insert({skey: 1, a: [1, 2]}));
    assert.commandWorked(mongos.getDB(db.getName()).coll.createIndex({skey: 1, a: 1}));

    // Create an index compatible the shard key
    assert.commandWorked(mongos.getDB(db.getName()).coll.createIndex({skey: 1}));

    st.shardColl(
        kSourceCollName, {skey: 1}, {skey: 0}, {skey: 1}, db.getName(), true /* waitForDelete */);

    // Connect directly to shards to bypass the mongos checks for dropping the non-multikey index
    assert.commandWorked(st.shard0.getDB(db.getName()).coll.dropIndex({skey: 1}));
    assert.commandWorked(st.shard1.getDB(db.getName()).coll.dropIndex({skey: 1}));

    // Database level mode command
    const inconsistencies = db.checkMetadataConsistency().toArray();
    assert.eq(2, inconsistencies.length);
    assert.eq("LastShardKeyIndexMultiKey", inconsistencies[0].type);
    assert.eq("LastShardKeyIndexMultiKey", inconsistencies[1].type);

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
})();

(function testClusterLevelMode() {
    const db_HiddenUnshardedCollection1 = getNewDb();
    const db_HiddenUnshardedCollection2 = getNewDb();
    const db_UUIDMismatch = getNewDb();

    // Insert HiddenUnshardedCollection inconsistency in db_HiddenUnshardedCollection1
    assert.commandWorked(mongos.adminCommand({
        enableSharding: db_HiddenUnshardedCollection1.getName(),
        primaryShard: st.shard0.shardName
    }));
    assert.commandWorked(
        st.shard1.getDB(db_HiddenUnshardedCollection1.getName()).coll.insert({_id: 'foo'}));

    // Insert HiddenUnshardedCollection inconsistency in db_HiddenUnshardedCollection2
    assert.commandWorked(mongos.adminCommand({
        enableSharding: db_HiddenUnshardedCollection2.getName(),
        primaryShard: st.shard1.shardName
    }));
    assert.commandWorked(
        st.shard0.getDB(db_HiddenUnshardedCollection2.getName()).coll.insert({_id: 'foo'}));

    // Insert UUIDMismatch inconsistency in db_UUIDMismatch
    assert.commandWorked(mongos.adminCommand(
        {enableSharding: db_UUIDMismatch.getName(), primaryShard: st.shard1.shardName}));

    assert.commandWorked(st.shard0.getDB(db_UUIDMismatch.getName()).coll.insert({_id: 'foo'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: db_UUIDMismatch.coll.getFullName(), key: {_id: 1}}));

    // Cluster level mode command
    const inconsistencies = mongos.getDB("admin").checkMetadataConsistency().toArray();

    // Check that there are 3 inconsistencies: 2 HiddenUnshardedCollection and 1 UUIDMismatch
    assert.eq(3, inconsistencies.length);
    const count = inconsistencies.reduce((acc, object) => {
        return object.type === "HiddenUnshardedCollection" ? acc + 1 : acc;
    }, 0);
    assert.eq(2, count);
    assert(inconsistencies.some(object => object.type === "UUIDMismatch"));

    // Clean up the databases to pass the hooks that detect inconsistencies
    db_HiddenUnshardedCollection1.dropDatabase();
    db_HiddenUnshardedCollection2.dropDatabase();
    db_UUIDMismatch.dropDatabase();
})();

st.stop();
})();
