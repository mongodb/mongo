/**
 * Tests that a collection with permitMigrations: false in config.collections prohibits committing a
 * moveChunk and disables the balancer.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_fcv_52,
 * ]
 */
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

const st = new ShardingTest({shards: 2, other: {chunkSize: 1}});
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

    const bigString = 'X'.repeat(1024 * 1024);  // 1MB

    // Split both collections into 4 chunks so balancing can occur.
    for (let coll of [collA, collB]) {
        coll.insert({_id: 1, s: bigString});
        coll.insert({_id: 10, s: bigString});
        coll.insert({_id: 20, s: bigString});
        coll.insert({_id: 30, s: bigString});

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

    jsTestLog(`Disabling balancing of ${collB.getFullName()} with setAllowMigrations ${
        setAllowMigrations} and parameters ${tojson(collBSetNoBalanceParam)}`);
    assert.commandWorked(
        configDB.collections.update({_id: collB.getFullName()}, {$set: collBSetNoBalanceParam}));

    setAllowMigrationsCmd(collB.getFullName(), setAllowMigrations);

    st.startBalancer();
    st.awaitBalance(collAName, dbName);
    st.stopBalancer();
    st.verifyCollectionIsBalanced(collA);

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
