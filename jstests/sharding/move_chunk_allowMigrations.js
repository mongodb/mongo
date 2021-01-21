/**
 * Tests that a collection with alloMigrations: false in config.collections prohibits committing a
 * moveChunk and disables the balancer.
 *
 * @tags: [
 *   requires_fcv_47,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/sharding/libs/find_chunks_util.js");

const st = new ShardingTest({config: 1, shards: 2});
const configDB = st.s.getDB("config");
const dbName = 'AllowMigrations';

// Resets database dbName and enables sharding and establishes shard0 as primary, test case agnostic
const setUpDb = function setUpDatabaseAndEnableSharding() {
    assert.commandWorked(st.s.getDB(dbName).dropDatabase());
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
};

// Tests that moveChunk does not succeed when {allowMigrations: false}
(function testAllowMigrationsFalsePreventsMoveChunk() {
    setUpDb();

    const collName = "collA";
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 0}));
    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 1}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Confirm that an inProgress moveChunk fails once {allowMigrations: false}
    const fp = configureFailPoint(st.shard0, "moveChunkHangAtStep5");
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            assert.commandFailedWithCode(
                db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                ErrorCodes.ConflictingOperationInProgress);
        }, ns, st.shard1.shardName), st.s.port);
    fp.wait();
    assert.commandWorked(
        configDB.collections.update({_id: ns}, {$set: {allowMigrations: false}}, {upsert: true}));
    fp.off();
    awaitResult();

    // {allowMigrations: false} is set, sending a new moveChunk command should also fail.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);

    // Confirm shard0 reports {allowMigrations: false} in the local cache as well
    const cachedEntry = st.shard0.getDB("config").cache.collections.findOne({_id: ns});
    assert.eq(false, cachedEntry.allowMigrations);
})();

// Tests {allowMigrations: false} disables balancing for collB and does not interfere with balancing
// for collA.
//
// collBSetParams specify the field(s) that will be set on the collB in config.collections.
const testBalancer = function testAllowMigrationsFalseDisablesBalancer(collBSetParams) {
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

        // Confirm the chunks are initially unbalanced. All chunks should start out on shard0
        // (primary shard for the database).
        const balancerStatus = assert.commandWorked(
            st.s0.adminCommand({balancerCollectionStatus: coll.getFullName()}));
        assert.eq(balancerStatus.balancerCompliant, false);
        assert.eq(balancerStatus.firstComplianceViolation, 'chunksImbalance');
        assert.eq(4,
                  findChunksUtil
                      .findChunksByNs(configDB, coll.getFullName(), {shard: st.shard0.shardName})
                      .count());
    }

    jsTestLog(
        `Disabling balancing of ${collB.getFullName()} with parameters ${tojson(collBSetParams)}`);
    assert.commandWorked(
        configDB.collections.update({_id: collB.getFullName()}, {$set: collBSetParams}));

    st.startBalancer();
    assert.soon(() => {
        st.awaitBalancerRound();
        const shard0Chunks =
            findChunksUtil
                .findChunksByNs(configDB, collA.getFullName(), {shard: st.shard0.shardName})
                .itcount();
        const shard1Chunks =
            findChunksUtil
                .findChunksByNs(configDB, collA.getFullName(), {shard: st.shard1.shardName})
                .itcount();
        jsTestLog(`shard0 chunks ${shard0Chunks}, shard1 chunks ${shard1Chunks}`);
        return shard0Chunks == 2 && shard1Chunks == 2;
    }, `Balancer failed to balance ${collA.getFullName()}`, 1000 * 60 * 10);
    st.stopBalancer();

    const collABalanceStatus =
        assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: collA.getFullName()}));
    assert.eq(collABalanceStatus.balancerCompliant, true);

    // Test that collB remains unbalanced.
    const collBBalanceStatus =
        assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: collB.getFullName()}));
    assert.eq(collBBalanceStatus.balancerCompliant, false);
    assert.eq(collBBalanceStatus.firstComplianceViolation, 'chunksImbalance');
    assert.eq(
        4,
        findChunksUtil.findChunksByNs(configDB, collB.getFullName(), {shard: st.shard0.shardName})
            .count());
};

// Test cases that should disable the balancer.
testBalancer({allowMigrations: false});
testBalancer({allowMigrations: false, noBalance: false});
testBalancer({allowMigrations: false, noBalance: true});

st.stop();
})();
