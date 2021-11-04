/**
 * Tests that a collection with allowMigrations: false in config.collections prohibits committing a
 * moveChunk and disables the balancer.
 * Also tests that the _configsvrSetAllowMigrations commands updates the 'allowMigrations' field and
 * bumps the collection version.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/sharding/libs/shard_versioning_util.js");

const st = new ShardingTest({shards: 2});
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
    const fp = configureFailPoint(st.shard0, "moveChunkHangAtStep4");
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            assert.commandFailedWithCode(
                db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                ErrorCodes.ConflictingOperationInProgress);
        }, ns, st.shard1.shardName), st.s.port);
    fp.wait();
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: false, writeConcern: {w: "majority"}}));
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

// TODO SERVER-61033: remove after permitMigrations have been merged with allowMigrations.
// Tests that moveChunk does not succeed when {permitMigrations: false}
(function testPermitMigrationsFalsePreventsMoveChunk() {
    setUpDb();

    const collName = "collA";
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 0}));
    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({_id: 1}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Confirm that an inProgress moveChunk fails once {allowMigrations: false}
    const fp = configureFailPoint(st.shard0, "moveChunkHangAtStep4");
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            assert.commandFailedWithCode(
                db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                ErrorCodes.ConflictingOperationInProgress);
        }, ns, st.shard1.shardName), st.s.port);
    fp.wait();
    assert.commandWorked(configDB.collections.updateOne(
        {_id: ns}, {$set: {permitMigrations: false}}, {writeConcern: {w: "majority"}}));
    fp.off();
    awaitResult();

    // {permitMigrations: false} is set, sending a new moveChunk command should also fail.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress);
})();

// Tests {allowMigrations: false} disables balancing for collB and does not interfere with balancing
// for collA.
//
// collBSetParams specify the field(s) that will be set on the collB in config.collections.
const testBalancer = function testAllowMigrationsFalseDisablesBalancer(allowMigrations,
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

    jsTestLog(`Disabling balancing of ${collB.getFullName()} with allowMigrations ${
        allowMigrations} and parameters ${tojson(collBSetNoBalanceParam)}`);
    assert.commandWorked(
        configDB.collections.update({_id: collB.getFullName()}, {$set: collBSetNoBalanceParam}));
    assert.commandWorked(st.configRS.getPrimary().adminCommand({
        _configsvrSetAllowMigrations: collB.getFullName(),
        allowMigrations: allowMigrations,
        writeConcern: {w: "majority"}
    }));

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

const testConfigsvrSetAllowMigrationsCommand = function() {
    setUpDb();

    const collName = "foo";
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

    ShardVersioningUtil.assertCollectionVersionEquals(st.shard0, ns, Timestamp(1, 0));

    // Use _configsvrSetAllowMigrations to forbid migrations from happening
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: false, writeConcern: {w: "majority"}}));

    // Check that allowMigrations has been set to 'false' on the configsvr config.collections.
    assert.eq(false, configDB.collections.findOne({_id: ns}).allowMigrations);

    // Check that the collection version has been bumped and the shard has refreshed.
    // TODO SERVER-59053 re-enable the following assert
    // ShardVersioningUtil.assertCollectionVersionEquals(st.shard0, ns, Timestamp(1, 1));

    // Use _configsvrSetAllowMigrations to allow migrations to happen
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: true, writeConcern: {w: "majority"}}));

    // Check that allowMigrations has been unset (that implies migrations are allowed) on the
    // configsvr config.collections.
    assert.eq(undefined, configDB.collections.findOne({_id: ns}).allowMigrations);

    // Check that the collection version has been bumped and the shard has refreshed.
    // TODO SERVER-59053 re-enable the following assert
    // ShardVersioningUtil.assertCollectionVersionEquals(st.shard0, ns, Timestamp(1, 2));

    // Check that _configsvrSetAllowMigrations validates the 'collectionUUID' parameter if passed
    const collectionUUID = configDB.collections.findOne({_id: ns}).uuid;
    const anotherUUID = UUID();

    assert.commandFailedWithCode(st.configRS.getPrimary().adminCommand({
        _configsvrSetAllowMigrations: ns,
        allowMigrations: false,
        collectionUUID: anotherUUID,
        writeConcern: {w: "majority"}
    }),
                                 ErrorCodes.InvalidUUID);
    assert.eq(undefined, configDB.collections.findOne({_id: ns}).allowMigrations);
    // TODO SERVER-59053 re-enable the following assert
    // ShardVersioningUtil.assertCollectionVersionEquals(st.shard0, ns, Timestamp(1, 2));

    assert.commandWorked(st.configRS.getPrimary().adminCommand({
        _configsvrSetAllowMigrations: ns,
        allowMigrations: false,
        collectionUUID: collectionUUID,
        writeConcern: {w: "majority"}
    }));
    assert.eq(false, configDB.collections.findOne({_id: ns}).allowMigrations);
    // TODO SERVER-59053 re-enable the following assert
    // ShardVersioningUtil.assertCollectionVersionEquals(st.shard0, ns, Timestamp(1, 3));
};

// Test cases that should disable the balancer.
testBalancer(false /* allowMigrations */, {});
testBalancer(false /* allowMigrations */, {noBalance: false});
testBalancer(false /* allowMigrations */, {noBalance: true});

// TODO SERVER-61033: merge permitMigrations with allowMigrations.
testBalancer(true /* allowMigrations */, {permitMigrations: false});

// Test the _configsvrSetAllowMigrations internal command
testConfigsvrSetAllowMigrationsCommand();

st.stop();
})();
