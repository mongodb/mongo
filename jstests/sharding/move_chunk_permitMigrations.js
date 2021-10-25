/**
 * Tests that a collection with permitMigrations: false in config.collections prohibits committing a
 * moveChunk and disables the balancer.
 *
 * @tags: [
 *   multiversion_incompatible
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const st = new ShardingTest({shards: 2});
const configDB = st.s.getDB("config");
const dbName = 'PermitMigrations';

// Resets database dbName and enables sharding and establishes shard0 as primary, test case agnostic
const setUpDb = function setUpDatabaseAndEnableSharding() {
    assert.commandWorked(st.s.getDB(dbName).dropDatabase());
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
};

const setPermitMigrations = function(ns, permit) {
    // For now update the flag manually, a user-facing command will be implemented with
    // SERVER-56227.
    assert.commandWorked(configDB.collections.updateOne(
        {_id: ns}, {$set: {permitMigrations: permit}}, {writeConcern: {w: "majority"}}));
};

// Tests that moveChunk does not succeed when {permitMigrations: false}
(function testPermitMigrationsFalsePreventsMoveChunk() {
    setUpDb();

    const collName = "collA";
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 0}));
    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 1}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Confirm that an inProgress moveChunk fails once {permitMigrations: false}
    const fp = configureFailPoint(st.shard0, "moveChunkHangAtStep4");
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            assert.commandFailedWithCode(
                db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                ErrorCodes.ConflictingOperationInProgress);
        }, ns, st.shard1.shardName), st.s.port);
    fp.wait();
    setPermitMigrations(ns, false);
    fp.off();
    awaitResult();

    // {permitMigrations: false} is set, sending a new moveChunk command should also fail.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);
})();

// Tests {permitMigrations: false} disables balancing for collB and does not interfere with
// balancing for collA.
//
// collBSetParams specify the field(s) that will be set on the collB in config.collections.
const testBalancer = function testPermitMigrationsFalseDisablesBalancer(permitMigrations,
                                                                        collBSetNoBalanceParam) {
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

    jsTestLog(`Disabling balancing of ${collB.getFullName()} with permitMigrations ${
        permitMigrations} and parameters ${tojson(collBSetNoBalanceParam)}`);
    assert.commandWorked(
        configDB.collections.update({_id: collB.getFullName()}, {$set: collBSetNoBalanceParam}));
    setPermitMigrations(collB.getFullName(), permitMigrations);

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

    assert.eq(
        4, configDB.chunks.countDocuments({ns: collB.getFullName(), shard: st.shard0.shardName}));
};

// Test cases that should disable the balancer.
testBalancer(false /* permitMigrations */, {});
testBalancer(false /* permitMigrations */, {noBalance: false});
testBalancer(false /* permitMigrations */, {noBalance: true});

st.stop();
})();
