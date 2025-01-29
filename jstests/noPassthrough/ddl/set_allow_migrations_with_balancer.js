/**
 * Tests that a collection with migrations not allowed is disregarded by the balancer,
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({shards: 2, other: {chunkSize: 1}});
const configDB = st.s.getDB("config");
const dbName = 'AllowMigrations';
const primaryShard = st.shard0.shardName;

const setUpDb = function setUpDatabaseAndEnableSharding() {
    assert.commandWorked(st.s.getDB(dbName).dropDatabase());
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard}));
};

const setAllowMigrationsCmd = function(ns, allow) {
    assert.commandWorked(st.s.adminCommand({setAllowMigrations: ns, allowMigrations: allow}));
};

// Test the effects of setAllowMigrations on the balancing of the targeted collection (having the
// 'noBalance' setting specified).
const testSetAllowMigrationsVsConfigureCollectionBalancing = function(noBalanceSetting) {
    setUpDb();

    const testCollName = "testColl";
    const testColl = st.s.getCollection(`${dbName}.${testCollName}`);

    const bigString = 'X'.repeat(1024 * 1024);  // 1MB

    // Split into 4 chunks so balancing can occur.
    testColl.insert({_id: 1, s: bigString});
    testColl.insert({_id: 10, s: bigString});
    testColl.insert({_id: 20, s: bigString});
    testColl.insert({_id: 30, s: bigString});

    assert.commandWorked(
        st.s.adminCommand({shardCollection: testColl.getFullName(), key: {_id: 1}}));

    assert.commandWorked(st.splitAt(testColl.getFullName(), {_id: 10}));
    assert.commandWorked(st.splitAt(testColl.getFullName(), {_id: 20}));
    assert.commandWorked(st.splitAt(testColl.getFullName(), {_id: 30}));

    // Confirm the chunks are initially unbalanced. All chunks should start out on shard0
    // (primary shard for the database).
    const balancerStatus = assert.commandWorked(
        st.s0.adminCommand({balancerCollectionStatus: testColl.getFullName()}));
    assert.eq(balancerStatus.balancerCompliant, false);
    assert.eq(balancerStatus.firstComplianceViolation, 'chunksImbalance');
    assert.eq(4,
              findChunksUtil.findChunksByNs(configDB, testColl.getFullName(), {shard: primaryShard})
                  .count());

    jsTestLog(
        `{setAllowMigrations: false} has higher priority than {noBalance: ${noBalanceSetting}}`);
    if (noBalanceSetting === null) {
        assert.commandWorked(
            configDB.collections.update({_id: testColl.getFullName()}, {$unset: {noBalance: 1}}));
    } else {
        assert.commandWorked(st.s.adminCommand(
            {configureCollectionBalancing: testColl.getFullName(), noBalance: noBalanceSetting}));
    }

    setAllowMigrationsCmd(testColl.getFullName(), false);

    // Test that testColl remains unbalanced.
    const testCollBalanceStatus =
        assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: testColl.getFullName()}));
    assert.eq(testCollBalanceStatus.balancerCompliant, false);
    assert.eq(testCollBalanceStatus.firstComplianceViolation, 'chunksImbalance');
    assert.eq(4,
              findChunksUtil.findChunksByNs(configDB, testColl.getFullName(), {shard: primaryShard})
                  .count());

    jsTestLog(`{setAllowMigrations: true} allows {noBalance: ${noBalanceSetting}'} to be applied`);
    setAllowMigrationsCmd(testColl.getFullName(), true);
    st.startBalancer();

    if (noBalanceSetting === null || noBalanceSetting === false) {
        st.awaitBalance(testCollName, dbName);
        st.stopBalancer();
        st.verifyCollectionIsBalanced(testColl);
    } else {
        st.awaitBalancerRound();
        st.stopBalancer();
        assert.eq(
            4,
            findChunksUtil.findChunksByNs(configDB, testColl.getFullName(), {shard: primaryShard})
                .count());
    }
};

testSetAllowMigrationsVsConfigureCollectionBalancing(null /*noBalanceSetting*/);
testSetAllowMigrationsVsConfigureCollectionBalancing(false /*noBalanceSetting*/);
testSetAllowMigrationsVsConfigureCollectionBalancing(true /*noBalanceSetting*/);

jsTest.log('setAllowMigration has only effects on the targeted namespace');
{
    setUpDb();

    const targetedCollName = "testColl";
    const targetedColl = st.s.getCollection(`${dbName}.${targetedCollName}`);
    const nonTargetedCollName = "nonTargetedColl";
    const nonTargetedColl = st.s.getCollection(`${dbName}.${nonTargetedCollName}`);

    const bigString = 'X'.repeat(1024 * 1024);  // 1MB

    // Split both collections into 4 chunks so balancing can occur.
    for (let coll of [nonTargetedColl, targetedColl]) {
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

        assert.commandWorked(st.splitAt(coll.getFullName(), {_id: 10}));
        assert.commandWorked(st.splitAt(coll.getFullName(), {_id: 20}));
        assert.commandWorked(st.splitAt(coll.getFullName(), {_id: 30}));
    }

    nonTargetedColl.insert({_id: 1, s: bigString});
    nonTargetedColl.insert({_id: 10, s: bigString});
    nonTargetedColl.insert({_id: 20, s: bigString});
    nonTargetedColl.insert({_id: 30, s: bigString});
    // Confirm the chunks are initially unbalanced. All chunks should start out on the collection
    // primary shard.
    const balancerStatus = assert.commandWorked(
        st.s0.adminCommand({balancerCollectionStatus: nonTargetedColl.getFullName()}));
    assert.eq(balancerStatus.balancerCompliant, false);
    assert.eq(balancerStatus.firstComplianceViolation, 'chunksImbalance');
    assert.eq(4,
              findChunksUtil
                  .findChunksByNs(configDB, nonTargetedColl.getFullName(), {shard: primaryShard})
                  .count());

    setAllowMigrationsCmd(targetedColl.getFullName(), false);

    st.startBalancer();
    st.awaitBalance(nonTargetedCollName, dbName);
    st.stopBalancer();
    st.verifyCollectionIsBalanced(nonTargetedColl);

    const nonTargetedCollBalanceStatus = assert.commandWorked(
        st.s.adminCommand({balancerCollectionStatus: nonTargetedColl.getFullName()}));
    assert.eq(nonTargetedCollBalanceStatus.balancerCompliant, true);
}

st.stop();
