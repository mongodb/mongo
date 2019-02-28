/**
 * Tests that high water mark and postBatchResumeTokens are handled correctly during upgrade from
 * and downgrade to both 3.6 and a pre-backport version of 4.0 on a sharded cluster.
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");                 // assertCreateCollection
    load("jstests/libs/fixture_helpers.js");                          // runCommandOnEachPrimary
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");  // supportsMajorityReadConcern
    load("jstests/multiVersion/libs/change_stream_hwm_helpers.js");   // ChangeStreamHWMHelpers
    load("jstests/multiVersion/libs/multi_cluster.js");               // upgradeCluster
    load("jstests/multiVersion/libs/multi_rs.js");                    // upgradeSet

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const preBackport40Version = ChangeStreamHWMHelpers.preBackport40Version;
    const latest40Version = ChangeStreamHWMHelpers.latest40Version;
    const latest36Version = ChangeStreamHWMHelpers.latest36Version;

    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 3},
        other: {
            mongosOptions: {binVersion: latest36Version},
            configOptions: {binVersion: latest36Version},
            rsOptions: {
                binVersion: latest36Version,
                setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
            },
        }
    });

    // Obtain references to the test database via mongoS and directly on shard0.
    let mongosDB = st.s.getDB(jsTestName());
    let primaryShard = st.rs0.getPrimary();

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
        [unshardedCollName]: () => st.s.getDB(jsTestName())[shardedCollName],
        [shardedCollName]: () => st.s.getDB(jsTestName())[unshardedCollName]
    };

    /**
     * Tests the behaviour of the cluster while upgrading from 'oldVersion' to 'latest40Version'.
     * The 'oldVersionFCVs' parameter specifies an array of FCVs supported by 'oldVersion'; the
     * upgrade/downgrade procedure will be tested with each of these FCVs.
     */
    function runUpgradeDowngradeTests(oldVersion, oldVersionFCVs) {
        for (let minFCV of oldVersionFCVs) {
            // Create a map to record the HWMs returned by each test type.
            const hwmTokenMap = {};

            // We start with the cluster running on 'oldVersion'. Should not see any PBRTs.
            jsTestLog(`Testing binary ${oldVersion} mongoS and shards, FCV ${minFCV}`);
            refreshCluster(oldVersion);
            assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: minFCV}));
            for (let collName in collMap) {
                const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens(
                    {coll: collMap[collName](), expectPBRT: false});
                assert.eq(hwmToken, undefined);
            }

            // Upgrade a single shard to 'latest40Version' but leave the mongoS on 'oldVersion'. No
            // streams produce PBRTs, but the new shard should continue to produce resumable tokens
            // and the old-style $sortKey while the cluster is mid-upgrade.
            jsTestLog(`Upgrading shard1 to binary ${latest40Version} FCV ${minFCV}`);
            refreshCluster(latest40Version, null, st.rs1);
            for (let collName in collMap) {
                const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens(
                    {coll: collMap[collName](), expectPBRT: false});
                assert.eq(hwmToken, undefined);
            }

            // Upgrade the remaining shard to 'latest40Version', but leave mongoS on 'oldVersion'.
            jsTestLog(
                `Upgrading to ${oldVersion} mongoS and ${latest40Version} shards, FCV ${minFCV}`);
            refreshCluster(latest40Version,
                           {upgradeMongos: false, upgradeShards: true, upgradeConfigs: true});

            // The shards are upgraded to 'latest40Version' but mongoS is running 'oldVersion'. The
            // mongoS should be able to merge the output from the shards, but neither mongoS stream
            // will produce a PBRT.
            jsTestLog(
                `Testing ${oldVersion} mongoS and ${latest40Version} shards with FCV ${minFCV}`);
            for (let collName in collMap) {
                const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens(
                    {coll: collMap[collName](), expectPBRT: false});
                assert.eq(hwmToken, undefined);
            }

            // Upgrade the mongoS to 'latest40Version' but leave the cluster in 'minFCV'.
            jsTestLog(
                `Upgrading to binary ${latest40Version} mongoS and shards with FCV ${minFCV}`);
            refreshCluster(latest40Version,
                           {upgradeMongos: true, upgradeShards: false, upgradeConfigs: false});

            // All streams should now return PBRTs, and we should obtain a valid HWM from the test.
            jsTestLog(`Testing binary ${latest40Version} mongoS and shards with FCV ${minFCV}`);
            for (let collName in collMap) {
                hwmTokenMap[collName] = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
                    coll: collMap[collName](),
                    expectPBRT: true,
                    hwmToResume: hwmTokenMap[collName],
                    expectResume: true
                });
                assert.neq(hwmTokenMap[collName], undefined);
            }

            // Set the cluster's FCV to 4.0.
            assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

            // All streams return PBRTs, and we can resume with the HWMs from the previous test.
            jsTestLog(`Testing binary ${latest40Version} mongoS and shards with FCV 4.0`);
            for (let collName in collMap) {
                hwmTokenMap[collName] = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
                    coll: collMap[collName](),
                    expectPBRT: true,
                    hwmToResume: hwmTokenMap[collName],
                    expectResume: true
                });
                assert.neq(hwmTokenMap[collName], undefined);
            }

            // Downgrade the cluster to 'minFCV'. We should continue to produce PBRTs and can still
            // resume from the tokens that we generated in the preceding tests.
            jsTestLog(`Downgrading to FCV ${minFCV} shards`);
            assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: minFCV}));

            jsTestLog(`Testing binary ${latest40Version} mongoS and shards with FCV ${minFCV}`);
            for (let collName in collMap) {
                hwmTokenMap[collName] = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
                    coll: collMap[collName](),
                    expectPBRT: true,
                    hwmToResume: hwmTokenMap[collName],
                    expectResume: true
                });
                assert.neq(hwmTokenMap[collName], undefined);
            }

            // Downgrade the mongoS to 'oldVersion'. We should be able to create new streams and
            // resume from their tokens, but cannot resume from the previously-generated v1 tokens.
            jsTestLog(`Downgrading to binary ${oldVersion} mongoS with FCV ${minFCV} shards`);
            refreshCluster(oldVersion,
                           {upgradeMongos: true, upgradeShards: false, upgradeConfigs: false});

            // We should no longer receive any PBRTs via mongoS, and we cannot resume from the HWMs.
            jsTestLog(`Testing binary ${oldVersion} mongoS and binary ${latest40Version} shards`);
            for (let collName in collMap) {
                const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
                    coll: collMap[collName](),
                    expectPBRT: false,
                    hwmToResume: hwmTokenMap[collName],
                    expectResume: false
                });
                assert.eq(hwmToken, undefined);
            }

            // Downgrade a single shard to 'oldVersion'. We should continue to observe the same
            // behaviour as we did in the previous test.
            jsTestLog(`Downgrading shard1 to binary ${oldVersion} FCV ${minFCV}`);
            refreshCluster(oldVersion, null, st.rs1);
            for (let collName in collMap) {
                const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
                    coll: collMap[collName](),
                    expectPBRT: false,
                    hwmToResume: hwmTokenMap[collName],
                    expectResume: false
                });
                assert.eq(hwmToken, undefined);
            }

            // Downgrade the remainder of the cluster to binary 'oldVersion'.
            jsTestLog(`Downgrading to binary ${oldVersion} shards`);
            refreshCluster(oldVersion,
                           {upgradeShards: true, upgradeConfigs: false, upgradeMongos: false});
            refreshCluster(oldVersion,
                           {upgradeConfigs: true, upgradeShards: false, upgradeMongos: false});

            // We should no longer receive any PBRTs, and we cannot resume from the HWM tokens.
            jsTestLog(`Testing downgraded binary ${oldVersion} mongoS and shards`);
            for (let collName in collMap) {
                const hwmToken = ChangeStreamHWMHelpers.testPostBatchAndHighWaterMarkTokens({
                    coll: collMap[collName](),
                    expectPBRT: false,
                    hwmToResume: hwmTokenMap[collName],
                    expectResume: false
                });
                assert.eq(hwmToken, undefined);
            }
        }
    }

    // Run the upgrade/downgrade tests from both 3.6 and pre-backport 4.0 versions.
    runUpgradeDowngradeTests(latest36Version, ["3.6"]);
    runUpgradeDowngradeTests(preBackport40Version, ["3.6", "4.0"]);

    st.stop();
})();