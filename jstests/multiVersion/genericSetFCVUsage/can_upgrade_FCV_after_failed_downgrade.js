/**
 * Tests for the new fcv change path added:
 * kDowngradingFromLatestToLastLTS -> kUpgradingFromLastLTSToLatest -> kLatest.
 *
 * @tags: [featureFlagDowngradingToUpgrading]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/feature_flag_util.js");

const latest = "latest";

function downgradingToUpgradingTest(conn, adminDB) {
    // 1) Startup: latest version.
    let fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("1) current FCV (should be latest):");
    printjson(fcvDoc);
    checkFCV(adminDB, latestFCV);

    // 2) Should be stuck in downgrading from latest to lastLTS.
    assert.commandWorked(  // failpoint: fail after transitional state.
        conn.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("2) current FCV (should be downgrading):");
    printjson(fcvDoc);
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV);

    // 3) FCV should be set to latest.
    assert.commandWorked(conn.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    let newFcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("3) current FCV (should be upgraded to latest):");
    printjson(fcvDoc);
    checkFCV(adminDB, latestFCV);

    // If config server, also check that timestamp from fcvDoc should be "earlier" (return -1).
    if (fcvDoc.changeTimestamp != null) {
        assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);
    }
}

function runStandaloneTest() {
    const conn = MongoRunner.runMongod({binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    const adminDB = conn.getDB("admin");

    if (!FeatureFlagUtil.isEnabled(adminDB, "DowngradingToUpgrading")) {
        jsTestLog("Skipping as featureFlagDowngradingToUpgrading is not enabled");
        MongoRunner.stopMongod(conn);
        return;
    }

    downgradingToUpgradingTest(conn, adminDB);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    const rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    const primaryAdminDB = rst.getPrimary().getDB("admin");
    const primary = rst.getPrimary();

    if (!FeatureFlagUtil.isEnabled(primaryAdminDB, "DowngradingToUpgrading")) {
        jsTestLog("Skipping as featureFlagDowngradingToUpgrading is not enabled");
        rst.stopSet();
        return;
    }

    downgradingToUpgradingTest(primary, primaryAdminDB);

    rst.stopSet();
}

function testConfigServerFCVTimestampIsAlwaysNewer() {
    const st =
        new ShardingTest({shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}}});
    const mongosAdminDB = st.s.getDB("admin");
    const configPrimary = st.configRS.getPrimary();
    const shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    checkFCV(shardPrimaryAdminDB, latestFCV);

    if (!FeatureFlagUtil.isEnabled(mongosAdminDB, "DowngradingToUpgrading")) {
        jsTestLog("Skipping as featureFlagDowngradingToUpgrading is not enabled");
        st.stop();
        return;
    }

    downgradingToUpgradingTest(configPrimary, mongosAdminDB);

    // Check that a new timestamp is always generated with each setFCV call.
    let fcvDoc;
    let newFcvDoc;
    // 1) Calling downgrade twice (one with failpoint).
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    fcvDoc = mongosAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    newFcvDoc = mongosAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    checkFCV(mongosAdminDB, lastLTSFCV);
    // Timestamp from fcvDoc should be less than the timestamp from newFcvDoc.
    assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);

    // 2) Calling upgrade twice (one with failpoint).
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failUpgrading', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    fcvDoc = mongosAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failUpgrading', mode: "off"}));
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    newFcvDoc = mongosAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    checkFCV(mongosAdminDB, latestFCV);
    // Timestamp from fcvDoc should be less than the timestamp from newFcvDoc.
    assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);

    st.stop();
}

// Helper for runShardingTest(): check that a sharded cluster can upgrade to latest fcv and
// assert all servers/router in the cluster have the latest fcv.
function setFCVToLatestSharding(
    mongosAdminDB, configPrimaryAdminDB, shard0PrimaryAdminDB, shard1PrimaryAdminDB) {
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(mongosAdminDB, latestFCV);
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shard0PrimaryAdminDB, latestFCV);
    checkFCV(shard1PrimaryAdminDB, latestFCV);
}

// Tests downgrading->upgrading on different failing scenarios for configServer or shards.
function runShardingTest() {
    const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
    const mongosAdminDB = st.s.getDB("admin");
    const configPrimary = st.configRS.getPrimary();
    const configPrimaryAdminDB = configPrimary.getDB("admin");
    const shard0Primary = st.rs0.getPrimary();
    const shard0PrimaryAdminDB = shard0Primary.getDB("admin");
    const shard1Primary = st.rs1.getPrimary();
    const shard1PrimaryAdminDB = shard1Primary.getDB("admin");

    // Make sure all servers start as latest version.
    setFCVToLatestSharding(
        mongosAdminDB, configPrimaryAdminDB, shard0PrimaryAdminDB, shard1PrimaryAdminDB);

    // 1. Test downgrading to upgraded where both shards failed initial downgrade.
    assert.commandWorked(
        shard0Primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandWorked(
        shard1Primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV);
    checkFCV(shard0PrimaryAdminDB, lastLTSFCV, lastLTSFCV);
    checkFCV(shard1PrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    assert.commandWorked(
        shard0Primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));
    assert.commandWorked(
        shard1Primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));
    setFCVToLatestSharding(
        mongosAdminDB, configPrimaryAdminDB, shard0PrimaryAdminDB, shard1PrimaryAdminDB);

    // 2. Test downgrading to upgraded with config in downgrading, both shards in upgraded.
    assert.commandWorked(configPrimary.adminCommand(
        {configureFailPoint: 'failBeforeSendingShardsToDowngrading', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV);
    checkFCV(shard0PrimaryAdminDB, latestFCV);
    checkFCV(shard1PrimaryAdminDB, latestFCV);

    assert.commandWorked(configPrimary.adminCommand(
        {configureFailPoint: 'failBeforeSendingShardsToDowngrading', mode: "off"}));
    setFCVToLatestSharding(
        mongosAdminDB, configPrimaryAdminDB, shard0PrimaryAdminDB, shard1PrimaryAdminDB);

    // 3. Test downgrading to upgraded path with 1 shard in downgrading + 1 shard in upgraded
    // (specifically: config and shard0 in downgrading, shard1 in upgraded/latest).
    assert.commandWorked(
        shard0Primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandWorked(shard1Primary.adminCommand(
        {configureFailPoint: 'failBeforeTransitioning', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV);
    checkFCV(shard0PrimaryAdminDB, lastLTSFCV, lastLTSFCV);
    checkFCV(shard1PrimaryAdminDB, latestFCV);

    assert.commandWorked(
        shard0Primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));
    assert.commandWorked(
        shard1Primary.adminCommand({configureFailPoint: 'failBeforeTransitioning', mode: "off"}));
    setFCVToLatestSharding(
        mongosAdminDB, configPrimaryAdminDB, shard0PrimaryAdminDB, shard1PrimaryAdminDB);

    // 4. Test downgrading to upgraded where both shards succeed initial downgrade but config
    // fails.
    assert.commandWorked(configPrimary.adminCommand(
        {configureFailPoint: 'failBeforeUpdatingFcvDoc', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV);
    checkFCV(shard0PrimaryAdminDB, lastLTSFCV);
    checkFCV(shard1PrimaryAdminDB, lastLTSFCV);

    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failBeforeUpdatingFcvDoc', mode: "off"}));
    setFCVToLatestSharding(
        mongosAdminDB, configPrimaryAdminDB, shard0PrimaryAdminDB, shard1PrimaryAdminDB);

    // 5. Test downgrading to upgraded where only one shard failed initial downgrade
    // (specifically: shard0 in downgraded, shard1 + config in downgrading).
    assert.commandWorked(shard1PrimaryAdminDB.adminCommand(
        {configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV);
    checkFCV(shard0PrimaryAdminDB, lastLTSFCV);
    checkFCV(shard1PrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    assert.commandWorked(
        shard1PrimaryAdminDB.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));
    setFCVToLatestSharding(
        mongosAdminDB, configPrimaryAdminDB, shard0PrimaryAdminDB, shard1PrimaryAdminDB);

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
testConfigServerFCVTimestampIsAlwaysNewer();
runShardingTest();
})();
