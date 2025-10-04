/**
 * Tests for the following fcv paths where an upgrade or downgrade fails an can be rolled back:
 * - Failed downgrading from latestFCV to lastLTSFCV, then upgrading back to latestFCV
 * - Failed upgrading from lastLTSFCV to latestFCV, then downgrading back to lastLTSFCV
 * - Failed upgrading from lastContinuousFCV to latestFCV, then downgrading back to
 * lastContinuousFCV
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const latest = "latest";
function revertUpgradeOrDowngradeToOriginalFCV(conn, adminDB, originalFCV, targetFCV) {
    const isOriginallyDowngrade = MongoRunner.compareBinVersions(originalFCV, targetFCV) === 1;
    const failPointName = isOriginallyDowngrade ? "failDowngrading" : "failUpgrading";
    if (!FeatureFlagUtil.isPresentAndEnabled(conn, "UpgradingToDowngrading") && !isOriginallyDowngrade) {
        return;
    }
    // Startup: set initial FCV.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV, confirm: true}));
    let fcvDoc = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog("1) Current FCV (should be original FCV):");
    printjson(fcvDoc);
    checkFCV(adminDB, originalFCV);

    // Should be stuck in transitional state using failpoint.
    assert.commandWorked(conn.adminCommand({configureFailPoint: failPointName, mode: "alwaysOn"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
    fcvDoc = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog("2) Current FCV (should be transitional state):");
    printjson(fcvDoc);
    checkFCV(adminDB, isOriginallyDowngrade ? targetFCV : originalFCV, targetFCV);

    // Reset failpoint, check FCV is reverted back to original state.
    assert.commandWorked(conn.adminCommand({configureFailPoint: failPointName, mode: "off"}));
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV, confirm: true}));
    let newFcvDoc = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog("3) Current FCV (should be reverted to original FCV):");
    printjson(newFcvDoc);
    checkFCV(adminDB, originalFCV);

    // Check timestamp consistency if applicable.
    if (fcvDoc.changeTimestamp != null) {
        assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);
    }

    // Reset FCV to latest for consistency across tests.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}
function runTests(conn, adminDB) {
    revertUpgradeOrDowngradeToOriginalFCV(conn, adminDB, latestFCV, lastLTSFCV);
    revertUpgradeOrDowngradeToOriginalFCV(conn, adminDB, lastLTSFCV, latestFCV);
    revertUpgradeOrDowngradeToOriginalFCV(conn, adminDB, lastContinuousFCV, latestFCV);
}

function runStandaloneTest() {
    const conn = MongoRunner.runMongod({binVersion: latest});
    assert.neq(null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    const adminDB = conn.getDB("admin");

    runTests(conn, adminDB);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    const primaryAdminDB = rst.getPrimary().getDB("admin");
    const primary = rst.getPrimary();

    runTests(primary, primaryAdminDB);

    rst.stopSet();
}

function testConfigServerFCVTimestampIsAlwaysNewer() {
    const st = new ShardingTest({shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}}});
    const mongosAdminDB = st.s.getDB("admin");
    const configPrimary = st.configRS.getPrimary();
    const shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    checkFCV(shardPrimaryAdminDB, latestFCV);

    runTests(configPrimary, mongosAdminDB);

    // Check that a new timestamp is always generated with each setFCV call.
    let fcvDoc;
    let newFcvDoc;
    // 1) Calling downgrade twice (one with failpoint).
    assert.commandWorked(configPrimary.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    fcvDoc = mongosAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    assert.commandWorked(configPrimary.adminCommand({configureFailPoint: "failDowngrading", mode: "off"}));
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    newFcvDoc = mongosAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    checkFCV(mongosAdminDB, lastLTSFCV);
    // Timestamp from fcvDoc should be less than the timestamp from newFcvDoc.
    assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);

    // 2) Calling upgrade twice (one with failpoint).
    assert.commandWorked(configPrimary.adminCommand({configureFailPoint: "failUpgrading", mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    fcvDoc = mongosAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    assert.commandWorked(configPrimary.adminCommand({configureFailPoint: "failUpgrading", mode: "off"}));
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    newFcvDoc = mongosAdminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    checkFCV(mongosAdminDB, latestFCV);
    // Timestamp from fcvDoc should be less than the timestamp from newFcvDoc.
    assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);

    st.stop();
}

//>>>Test specific scenarios in sharded clusters

// Helper for runShardingTest(): check that a sharded cluster can upgrade to latestFCV
// or downgrade to lastLTS/lastContinuous and assert all servers/router in the cluster have the same
// fcv.
function setAndCheckFCV(FCV, adminDBs) {
    assert.commandWorked(adminDBs.mongos.runCommand({setFeatureCompatibilityVersion: FCV, confirm: true}));
    checkFCV(adminDBs.mongos, FCV);
    checkFCV(adminDBs.config, FCV);
    checkFCV(adminDBs.shard0, FCV);
    checkFCV(adminDBs.shard1, FCV);
}

/**
 * Case where both shards fail during initial transition
 */
function testBothShardsFail(originalFCV, targetFCV, adminDBs) {
    const isDowngrade = MongoRunner.compareBinVersions(originalFCV, targetFCV) === 1;
    const effectiveFCV = isDowngrade ? targetFCV : originalFCV;

    jsTestLog("Running test: Both shards fail during initial transition");

    setAndCheckFCV(originalFCV, adminDBs);

    const failPointName = isDowngrade ? "failDowngrading" : "failUpgrading";
    assert.commandWorked(adminDBs.shard0.runCommand({configureFailPoint: failPointName, mode: "alwaysOn"}));
    assert.commandWorked(adminDBs.shard1.runCommand({configureFailPoint: failPointName, mode: "alwaysOn"}));

    assert.commandFailed(adminDBs.mongos.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));

    checkFCV(adminDBs.config, effectiveFCV, targetFCV);
    checkFCV(adminDBs.shard0, effectiveFCV, targetFCV);
    checkFCV(adminDBs.shard1, effectiveFCV, targetFCV);

    assert.commandWorked(adminDBs.shard0.runCommand({configureFailPoint: failPointName, mode: "off"}));
    assert.commandWorked(adminDBs.shard1.runCommand({configureFailPoint: failPointName, mode: "off"}));

    setAndCheckFCV(originalFCV, adminDBs);

    // Reset to latestFCV for consistency across tests
    setAndCheckFCV(latestFCV, adminDBs);
}

/**
 * Case where config server fails before transitioning shards.
 */
function testConfigFails(originalFCV, targetFCV, adminDBs) {
    const isDowngrade = MongoRunner.compareBinVersions(originalFCV, targetFCV) === 1;
    const effectiveFCV = isDowngrade ? targetFCV : originalFCV;

    jsTestLog("Running test: Config server fails before transitioning shards");

    setAndCheckFCV(originalFCV, adminDBs);

    assert.commandWorked(
        adminDBs.config.runCommand({
            configureFailPoint: "failBeforeSendingShardsToDowngradingOrUpgrading",
            mode: "alwaysOn",
        }),
    );

    assert.commandFailed(adminDBs.mongos.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));

    checkFCV(adminDBs.config, effectiveFCV, targetFCV);
    checkFCV(adminDBs.shard0, originalFCV);
    checkFCV(adminDBs.shard1, originalFCV);

    assert.commandWorked(
        adminDBs.config.runCommand({
            configureFailPoint: "failBeforeSendingShardsToDowngradingOrUpgrading",
            mode: "off",
        }),
    );

    setAndCheckFCV(originalFCV, adminDBs);

    // Reset to latestFCV for consistency across tests
    setAndCheckFCV(latestFCV, adminDBs);
}

/**
 * Case where one shard fails during transition and other fails before transitioning
 */
function testMixedFailures(originalFCV, targetFCV, adminDBs) {
    const isDowngrade = MongoRunner.compareBinVersions(originalFCV, targetFCV) === 1;
    const effectiveFCV = isDowngrade ? targetFCV : originalFCV;
    jsTestLog("Running test: Mixed failures between config and shards");

    setAndCheckFCV(originalFCV, adminDBs);

    const failPointName = isDowngrade ? "failDowngrading" : "failUpgrading";
    assert.commandWorked(adminDBs.shard0.runCommand({configureFailPoint: failPointName, mode: "alwaysOn"}));
    assert.commandWorked(adminDBs.shard1.runCommand({configureFailPoint: "failBeforeTransitioning", mode: "alwaysOn"}));

    assert.commandFailed(adminDBs.mongos.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));

    checkFCV(adminDBs.config, effectiveFCV, targetFCV);
    checkFCV(adminDBs.shard0, effectiveFCV, targetFCV);
    checkFCV(adminDBs.shard1, originalFCV);

    assert.commandWorked(adminDBs.shard0.runCommand({configureFailPoint: failPointName, mode: "off"}));
    assert.commandWorked(adminDBs.shard1.runCommand({configureFailPoint: "failBeforeTransitioning", mode: "off"}));

    setAndCheckFCV(originalFCV, adminDBs);

    // Reset to latestFCV for consistency across tests
    setAndCheckFCV(latestFCV, adminDBs);
}

function runShardingTests() {
    const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
    const adminDBs = {
        mongos: st.s.getDB("admin"),
        config: st.configRS.getPrimary().getDB("admin"),
        shard0: st.rs0.getPrimary().getDB("admin"),
        shard1: st.rs1.getPrimary().getDB("admin"),
    };

    const testCases = [{originalFCV: latestFCV, targetFCV: lastLTSFCV}];
    if (FeatureFlagUtil.isPresentAndEnabled(adminDBs.mongos, "UpgradingToDowngrading")) {
        testCases.push(
            {originalFCV: lastLTSFCV, targetFCV: latestFCV},
            {originalFCV: lastContinuousFCV, targetFCV: latestFCV},
        );
    }

    testCases.forEach(({originalFCV, targetFCV}) => {
        testBothShardsFail(originalFCV, targetFCV, adminDBs);
        testConfigFails(originalFCV, targetFCV, adminDBs);
        testMixedFailures(originalFCV, targetFCV, adminDBs);
    });

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
testConfigServerFCVTimestampIsAlwaysNewer();
runShardingTests();
