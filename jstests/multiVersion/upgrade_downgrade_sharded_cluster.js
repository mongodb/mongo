/**
 * The goal of this test is to verify that some metadata is properly updated when
 * upgrading/downgrading a sharded cluster. More specifically:
 *
 *	1. We create a sharded cluster running and old binary version (lastLTSFCV or lastContinuousFCV)
 *	2. We setup some state on cluster
 *	3. We upgrade the binaries of the sharded cluster to the latest version + set FCV to latestFCV
 *	4. We verify that upgrade procedures have been performed
 *	5. We set FCV to old bin version + downgrade the binaries of the sharded cluster to that version
 *	6. We verify that downgrade procedures have been performed
 *
 * @tags: [disabled_due_to_server_58295]
 */
(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_cluster.js');  // for upgradeCluster()

function setupInitialStateOnOldVersion(oldVersion) {
}

function checkConfigAndShardsFCV(expectedFCV) {
    var configFCV = st.configRS.getPrimary()
                        .adminCommand({getParameter: 1, featureCompatibilityVersion: 1})
                        .featureCompatibilityVersion.version;
    assert.eq(expectedFCV, configFCV);

    var shard0FCV = st.rs0.getPrimary()
                        .adminCommand({getParameter: 1, featureCompatibilityVersion: 1})
                        .featureCompatibilityVersion.version;
    assert.eq(expectedFCV, shard0FCV);

    var shard1FCV = st.rs1.getPrimary()
                        .adminCommand({getParameter: 1, featureCompatibilityVersion: 1})
                        .featureCompatibilityVersion.version;
    assert.eq(expectedFCV, shard1FCV);
}

function runChecksAfterUpgrade() {
    checkConfigAndShardsFCV(latestFCV);
}

function setupStateBeforeDowngrade() {
}

function runChecksAfterFCVDowngrade(oldVersion) {
    checkConfigAndShardsFCV(oldVersion);
}

function runChecksAfterBinDowngrade() {
}

for (let oldVersion of [lastLTSFCV, lastContinuousFCV]) {
    var st = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            mongosOptions: {binVersion: oldVersion},
            configOptions: {binVersion: oldVersion},
            shardOptions: {binVersion: oldVersion},

            rsOptions: {binVersion: oldVersion},
            rs: true,
        }
    });

    jsTest.log('oldVersion: ' + oldVersion);

    st.configRS.awaitReplication();

    // Setup initial conditions
    setupInitialStateOnOldVersion(oldVersion);

    // Upgrade the entire cluster to the latest version.
    jsTest.log('upgrading cluster binaries');
    st.upgradeCluster(latestFCV);

    jsTest.log('upgrading cluster FCV');
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Tests after upgrade
    runChecksAfterUpgrade();

    // Setup state before downgrade
    setupStateBeforeDowngrade();

    // Downgrade FCV back to oldVersion
    jsTest.log('downgrading cluster FCV');
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion}));

    // Tests after FCV downgrade to oldVersion
    runChecksAfterFCVDowngrade(oldVersion);

    // Downgrade binaries back to oldVersion
    jsTest.log('downgrading cluster binaries');
    st.upgradeCluster(oldVersion);

    // Tests after binaries downgrade to oldVersion
    runChecksAfterBinDowngrade();

    st.stop();
}
})();
