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

(function testUUIDMismatchInconsistency() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: db.coll.getFullName(), key: {_id: 1}}));

    // Database level mode command
    const res = db.runCommand({checkMetadataConsistency: 1});
    assert.commandWorked(res);
    assert.eq(res.inconsistencies.length, 1);
    assert.eq(res.inconsistencies[0].type, "UUIDMismatch");

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
})();

(function testHiddenUnshardedCollection() {
    const db = getNewDb();

    assert.commandWorked(
        mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.shard1.getDB(db.getName()).coll.insert({_id: 'foo'}));

    // Database level mode command
    const res = db.runCommand({checkMetadataConsistency: 1});
    assert.commandWorked(res);
    assert.eq(res.inconsistencies.length, 1);
    assert.eq(res.inconsistencies[0].type, "HiddenUnshardedCollection");

    // Clean up the database to pass the hooks that detect inconsistencies
    db.dropDatabase();
})();

st.stop();
})();
