/*
 * Tests to validate the correct behaviour of checkMetadataConsistency command.
 *
 * @tags: [featureFlagCheckMetadataConsistency]
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

    // Cluster level mode command
    assert.commandFailedWithCode(st.s.adminCommand({checkMetadataConsistency: 1}),
                                 ErrorCodes.NotImplemented);

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

st.stop();
})();
