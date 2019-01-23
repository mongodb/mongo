/**
 * Tests that high water mark and postBatchResumeTokens are handled correctly during upgrade from
 * and downgrade to a pre-backport version of 4.0 on a sharded cluster.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");                 // assertCreateCollection
    load("jstests/libs/fixture_helpers.js");                          // runCommandOnEachPrimary
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");  // supportsMajorityReadConcern
    load("jstests/multiVersion/libs/change_stream_hwm_helpers.js");   // ChangeStreamHWMHelpers
    load("jstests/multiVersion/libs/index_format_downgrade.js");      // downgradeUniqueIndexes
    load("jstests/multiVersion/libs/multi_cluster.js");               // upgradeCluster
    load("jstests/multiVersion/libs/multi_rs.js");                    // upgradeSet

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const preBackport40Version = ChangeStreamHWMHelpers.preBackport40Version;
    const latest42Version = ChangeStreamHWMHelpers.latest42Version;

    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 3},
        other: {
            mongosOptions: {binVersion: preBackport40Version},
            configOptions: {binVersion: preBackport40Version},
            rsOptions: {
                binVersion: preBackport40Version,
                setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
            },
        }
    });

    // Obtain references to the test database via mongoS and directly on shard0.
    let mongosDB = st.s.getDB(jsTestName());
    let primaryShard = st.rs0.getPrimary();
    let primaryShardDB = primaryShard.getDB(jsTestName());

    // Names of each of the collections used in the course of this test.
    const shardedCollName = "sharded_coll";
    const unshardedCollName = "unsharded_coll";

    // Updates the specified cluster components and then refreshes our references to each of them.
    function refreshCluster(version, components, singleShard) {
        if (singleShard) {
            singleShard.upgradeSet({binVersion: version});
        } else {
            st.upgradeCluster(version, components);
        }

        // Wait for the config server and shards to become available, and restart mongoS.
        st.configRS.awaitReplication();
        st.rs0.awaitReplication();
        st.rs1.awaitReplication();
        st.restartMongoses();

        // Having upgraded the cluster, reacquire references to each component.
        mongosDB = st.s.getDB(jsTestName());
        primaryShard = st.rs0.getPrimary();
        primaryShardDB = primaryShard.getDB(jsTestName());

        // Re-apply the 'writePeriodicNoops' parameter to the up/downgraded shards.
        const mongosAdminDB = mongosDB.getSiblingDB("admin");
        FixtureHelpers.runCommandOnEachPrimary(
            {db: mongosAdminDB, cmdObj: {setParameter: 1, writePeriodicNoops: true}});
    }

    // Enable sharding on the the test database and ensure that the primary is shard0.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), primaryShard.name);

    // Create an unsharded collection on the primary shard via mongoS.
    assertCreateCollection(mongosDB, unshardedCollName);

    // Create a sharded collection on {_id: 1}, split across the shards at {_id: 0}.
    const collToShard = assertCreateCollection(mongosDB, shardedCollName);
    st.shardColl(collToShard, {_id: 1}, {_id: 0}, {_id: 1});

    // Maps used to associate collection names with collection objects and HWM tokens.
    const collMap = {
        [unshardedCollName]: () => st.s.getDB(jsTestName()).mongosUnshardedColl,
        [shardedCollName]: () => st.s.getDB(jsTestName()).mongosShardedColl
    };
    const hwmTokenMap = {};

    // We start with the cluster running on 'preBackport40Version'. No streams should produce PBRTs.
    jsTestLog("Testing binary 4.0 mongoS and shards");
    for (let collName in collMap) {
        const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens(
            {coll: collMap[collName](), expectPBRT: false});
        assert.eq(hwmToken, undefined);
    }

    // Upgrade a single shard to 4.2 but leave the mongoS on 4.0. No streams produce PBRTs, but the
    // new shard should continue to produce resumable v0 tokens and the old-style $sortKey while the
    // cluster is mid-upgrade.
    jsTestLog("Upgrading shard1 to binary 4.2 FCV 4.0");
    refreshCluster(latest42Version, null, st.rs1);
    for (let collName in collMap) {
        const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens(
            {coll: collMap[collName](), expectPBRT: false});
        assert.eq(hwmToken, undefined);
    }

    // Upgrade the remaining shard to 4.2 but leave the mongoS on 4.0.
    jsTestLog("Upgrading to binary 4.0 mongoS and binary 4.2 shards with FCV 4.0");
    refreshCluster(latest42Version,
                   {upgradeMongos: false, upgradeShards: true, upgradeConfigs: true});

    // The shards have been upgraded to 4.2 but the mongoS is running 4.0. The mongoS should be able
    // to merge the output from the shards, but neither of the mongoS streams will generate a PBRT.
    jsTestLog("Testing binary 4.0 mongoS and binary 4.2 shards with FCV 4.0");
    for (let collName in collMap) {
        const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens(
            {coll: collMap[collName](), expectPBRT: false});
        assert.eq(hwmToken, undefined);
    }

    // Upgrade the mongoS to 4.2 but leave the cluster in FCV 4.0
    jsTestLog("Upgrading to binary 4.2 mongoS and shards with FCV 4.0");
    refreshCluster(latest42Version,
                   {upgradeMongos: true, upgradeShards: false, upgradeConfigs: false});

    // All streams should now return PBRTs, and we should obtain a valid HWM from the test.
    jsTestLog("Testing binary 4.2 mongoS and shards with FCV 4.0");
    for (let collName in collMap) {
        hwmTokenMap[collName] = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
            coll: collMap[collName](),
            expectPBRT: true,
            hwmToResume: hwmTokenMap[collName],
            expectResume: true
        });
        assert.neq(hwmTokenMap[collName], undefined);
    }

    // Set the cluster's FCV to 4.2.
    assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

    // All streams should now return PBRTs; we can resume from all HWMs tokens in the previous test.
    jsTestLog("Testing binary 4.2 mongoS and shards with FCV 4.2");
    for (let collName in collMap) {
        hwmTokenMap[collName] = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
            coll: collMap[collName](),
            expectPBRT: true,
            hwmToResume: hwmTokenMap[collName],
            expectResume: true
        });
        assert.neq(hwmTokenMap[collName], undefined);
    }

    // Downgrade the cluster to FCV 4.0. We should continue to produce PBRTs and can resume from the
    // tokens that we generated previously.
    jsTestLog("Downgrading to FCV 4.0 shards");
    assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    jsTestLog("Testing binary 4.2 mongoS and shards with FCV 4.0");
    for (let collName in collMap) {
        hwmTokenMap[collName] = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
            coll: collMap[collName](),
            expectPBRT: true,
            hwmToResume: hwmTokenMap[collName],
            expectResume: true
        });
        assert.neq(hwmTokenMap[collName], undefined);
    }

    // Downgrade the mongoS to 'preBackport40Version'. We should be able to create new streams and
    // resume from their tokens, but cannot resume from the previously-generated v1 tokens.
    jsTestLog("Downgrading to binary 4.0 mongoS with FCV 4.0 shards");
    refreshCluster(preBackport40Version,
                   {upgradeMongos: true, upgradeShards: false, upgradeConfigs: false});

    // We should no longer receive any PBRTs via mongoS, and we cannot resume from the HWM tokens.
    jsTestLog("Testing downgraded binary 4.0 mongoS and shards");
    for (let collName in collMap) {
        const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
            coll: collMap[collName](),
            expectPBRT: false,
            hwmToResume: hwmTokenMap[collName],
            expectResume: false
        });
        assert.eq(hwmToken, undefined);
    }

    // Downgrade a single shard to binary 4.0, after rebuilding all unique indexes so that their
    // format is compatible. We should continue to observe the same behaviour as we did in the
    // previous test.
    jsTestLog("Downgrading shard1 to binary 4.0");
    downgradeUniqueIndexes(mongosDB);
    refreshCluster(preBackport40Version, null, st.rs1);
    jsTestLog("Testing downgraded shard1 with binary 4.0");
    for (let collName in collMap) {
        const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
            coll: collMap[collName](),
            expectPBRT: false,
            hwmToResume: hwmTokenMap[collName],
            expectResume: false
        });
        assert.eq(hwmToken, undefined);
    }

    // Downgrade the remainder of the cluster to binary 4.0.
    jsTestLog("Downgrading to binary 4.0 shards");
    refreshCluster(preBackport40Version,
                   {upgradeShards: true, upgradeConfigs: false, upgradeMongos: false});
    refreshCluster(preBackport40Version,
                   {upgradeConfigs: true, upgradeShards: false, upgradeMongos: false});

    // We should no longer receive any PBRTs, and we cannot resume from the HWM tokens we generated.
    jsTestLog("Testing downgraded binary 4.0 mongoS and shards");
    for (let collName in collMap) {
        const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
            coll: collMap[collName](),
            expectPBRT: false,
            hwmToResume: hwmTokenMap[collName],
            expectResume: false
        });
        assert.eq(hwmToken, undefined);
    }

    st.stop();
})();