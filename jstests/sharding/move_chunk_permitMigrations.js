/**
 * Tests that a collection with permitMigrations: false in config.collections prohibits committing a
 * moveChunk and disables the balancer.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const st = new ShardingTest({shards: 2});
const configDB = st.s.getDB("config");
const dbName = 'AllowMigrations';

// Resets database dbName and enables sharding and establishes shard0 as primary, test case agnostic
const setUpDb = function setUpDatabaseAndEnableSharding() {
    assert.commandWorked(st.s.getDB(dbName).dropDatabase());
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
};

// Use the setAllowMigrations command to set the permitMigrations flag in the collection.
const setAllowMigrationsCmd = function(ns, allow) {
    assert.commandWorked(st.s.adminCommand({setAllowMigrations: ns, allowMigrations: allow}));
};

// Tests that moveChunk does not succeed when setAllowMigrations is called with a false value.
(function testSetAllowMigrationsFalsePreventsMoveChunk() {
    setUpDb();

    const collName = "collA";
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 0}));
    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 1}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    setAllowMigrationsCmd(ns, false);

    // setAllowMigrations was called, sending a new moveChunk command should fail.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);
})();

// Tests setAllowMigrations disables balancing for collB and does not interfere with
// balancing for collA.
//
// collBSetParams specify the field(s) that will be set on the collB in config.collections.
const testBalancer = function(setAllowMigrations, collBSetNoBalanceParam) {
    setUpDb();

    const collAName = "collA";
    const collBName = "collB";
    const collA = st.s.getCollection(`${dbName}.${collAName}`);
    const collB = st.s.getCollection(`${dbName}.${collBName}`);

    assert.commandWorked(st.s.adminCommand({shardCollection: collA.getFullName(), key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({shardCollection: collB.getFullName(), key: {_id: 1}}));

    // Split both collections into 4 chunks so balancing can occur.
    for (let coll of [collA, collB]) {
        coll.insert({_id: 1});
        coll.insert({_id: 10});
        coll.insert({_id: 20});
        coll.insert({_id: 30});

        assert.commandWorked(st.splitAt(coll.getFullName(), {_id: 10}));
        assert.commandWorked(st.splitAt(coll.getFullName(), {_id: 20}));
        assert.commandWorked(st.splitAt(coll.getFullName(), {_id: 30}));

        assert.eq(
            4,
            configDB.chunks.countDocuments({ns: coll.getFullName(), shard: st.shard0.shardName}));
    }

    jsTestLog(`Disabling balancing of ${collB.getFullName()} with setAllowMigrations ${
        setAllowMigrations} and parameters ${tojson(collBSetNoBalanceParam)}`);
    assert.commandWorked(
        configDB.collections.update({_id: collB.getFullName()}, {$set: collBSetNoBalanceParam}));

    setAllowMigrationsCmd(collB.getFullName(), setAllowMigrations);

    st.startBalancer();
    assert.soon(() => {
        st.awaitBalancerRound();
        const shard0Chunks =
            configDB.chunks.countDocuments({ns: collA.getFullName(), shard: st.shard0.shardName});
        const shard1Chunks =
            configDB.chunks.countDocuments({ns: collA.getFullName(), shard: st.shard1.shardName});
        jsTestLog(`shard0 chunks ${shard0Chunks}, shard1 chunks ${shard1Chunks}`);
        return shard0Chunks == 2 && shard1Chunks == 2;
    }, `Balancer failed to balance ${collA.getFullName()}`, 1000 * 60 * 10);
    st.stopBalancer();

    // collB should still be unbalanced.
    assert.eq(
        4, configDB.chunks.countDocuments({ns: collB.getFullName(), shard: st.shard0.shardName}));
};

const testSetAllowMigrationsCommand = function() {
    setUpDb();

    const collName = "foo";
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

    // Use setAllowMigrations to forbid migrations from happening
    setAllowMigrationsCmd(ns, false);

    // Check that allowMigrations has been set to 'false' on the configsvr config.collections.
    assert.eq(false, configDB.collections.findOne({_id: ns}).permitMigrations);

    // Use setAllowMigrations to allow migrations to happen
    setAllowMigrationsCmd(ns, true);

    // Check that permitMigrations has been unset (that implies migrations are allowed) on the
    // configsvr config.collections.
    assert.eq(undefined, configDB.collections.findOne({_id: ns}).permitMigrations);
};

// Test cases that should disable the balancer.
testBalancer(false /* setAllowMigrations */, {});
testBalancer(false /* setAllowMigrations */, {noBalance: false});
testBalancer(false /* setAllowMigrations */, {noBalance: true});

// Test the setAllowMigrations command.
testSetAllowMigrationsCommand();

st.stop();
})();
