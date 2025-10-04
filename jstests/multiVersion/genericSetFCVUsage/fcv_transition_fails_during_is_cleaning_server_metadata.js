/**
 * Test that:
 * 1. If a FCV downgrade fails during the isCleaningServerMetadata phase, starting an FCV upgrade is
 * disallowed until the downgrade is completed.
 * 2. If a FCV upgrade fails during the isCleaningServerMetadata phase, starting an FCV downgrade is
 * disallowed until the upgrade is completed.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const latest = "latest";
const testName = jsTestName();
const dbpath = MongoRunner.dataPath + testName;

function runTest(conn, restartDeploymentFn, configureFailPointFn) {
    // Always run the originally downgrading test.
    conn = transitionFailsDuringIsCleaningServerMetadata(conn, restartDeploymentFn, configureFailPointFn, "downgrade");

    // Only run the originally upgrading test if the feature flag is enabled.
    if (FeatureFlagUtil.isPresentAndEnabled(conn, "UpgradingToDowngrading")) {
        transitionFailsDuringIsCleaningServerMetadata(conn, restartDeploymentFn, configureFailPointFn, "upgrade");
    }
}

function transitionFailsDuringIsCleaningServerMetadata(
    conn,
    restartDeploymentFn,
    configureFailPointFn,
    originalTransitionType,
) {
    const isOriginallyDowngradeTest = originalTransitionType === "downgrade";
    const targetFCV = isOriginallyDowngradeTest ? lastLTSFCV : latestFCV;
    const originalFCV = isOriginallyDowngradeTest ? latestFCV : lastLTSFCV;
    const cannotTransitionDuringMetadataCleanupErrorCode = isOriginallyDowngradeTest ? 7428200 : 10778001;

    let adminDB = conn.getDB("admin");
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV, confirm: true}));

    configureFailPointFn("failTransitionDuringIsCleaningServerMetadata", "alwaysOn");

    // Fail while cleaning server metadata during the initial transition to the target FCV.
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
    let fcvDoc = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog("1. Current FCV should be in isCleaningServerMetadata phase: " + tojson(fcvDoc));
    checkFCV(adminDB, lastLTSFCV, targetFCV, true /* isCleaningServerMetadata */);

    // Transition in the opposite direction fails (i.e. trying to roll back to the original FCV).
    assert.commandFailedWithCode(
        adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV, confirm: true}),
        cannotTransitionDuringMetadataCleanupErrorCode,
    );

    configureFailPointFn("failTransitionDuringIsCleaningServerMetadata", "off");

    // We are still in isCleaningServerMetadata even if retrying transition fails at an earlier
    // point.
    const earlierFailPoint = isOriginallyDowngradeTest ? "failDowngrading" : "failUpgrading";
    jsTestLog(`Test that retrying ${originalTransitionType} and failing at an earlier point still keeps the error.`);
    configureFailPointFn(earlierFailPoint, "alwaysOn");
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
    fcvDoc = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog("2. Current FCV should still be in isCleaningServerMetadata phase: " + tojson(fcvDoc));
    checkFCV(adminDB, lastLTSFCV, targetFCV, true /* isCleaningServerMetadata */);

    // Assert transition in the opposite direction fails again.
    assert.commandFailedWithCode(
        adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV, confirm: true}),
        cannotTransitionDuringMetadataCleanupErrorCode,
    );

    jsTestLog("isCleaningServerMetadata should persist through restarts.");
    conn = restartDeploymentFn();
    adminDB = conn.getDB("admin");

    fcvDoc = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
    jsTestLog("3. Current FCV should still be in isCleaningServerMetadata phase: " + tojson(fcvDoc));
    checkFCV(adminDB, lastLTSFCV, targetFCV, true /* isCleaningServerMetadata */);

    // Assert transition in the opposite direction still fails after restarting.
    assert.commandFailedWithCode(
        adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV, confirm: true}),
        cannotTransitionDuringMetadataCleanupErrorCode,
    );

    // Ensure the original failing transition can be completed.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
    checkFCV(adminDB, targetFCV);

    // Now opposite transition succeeds.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV, confirm: true}));
    checkFCV(adminDB, originalFCV);

    return conn;
}

function runStandaloneTest() {
    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});

    assert.neq(null, conn, "mongod was unable to start up with version=" + latest + " and no data files");

    function restartDeploymentFn() {
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
        assert.neq(null, conn, "mongod was unable to start up with version=" + latest + " and existing data files");
        return conn;
    }

    function configureFailPointFn(failPointName, mode) {
        assert.commandWorked(conn.getDB("admin").runCommand({configureFailPoint: failPointName, mode: mode}));
    }

    jsTestLog("Running standalone tests.");
    runTest(conn, restartDeploymentFn, configureFailPointFn);
    jsTestLog("Standalone tests completed successfully");

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: latest}});
    rst.startSet();
    rst.initiate();

    function restartDeploymentFn() {
        rst.stopSet(null /* signal */, true /* forRestart */);
        rst.startSet({restart: true});
        return rst.getPrimary();
    }

    function configureFailPointFn(failPointName, mode) {
        assert.commandWorked(
            rst.getPrimary().getDB("admin").runCommand({configureFailPoint: failPointName, mode: mode}),
        );
    }

    jsTestLog("Running replica set tests.");
    runTest(rst.getPrimary(), restartDeploymentFn, configureFailPointFn);
    jsTestLog("Replica set tests completed successfully");

    rst.stopSet();
}

function runShardedClusterTest() {
    const st = new ShardingTest({shards: 2});

    function restartDeploymentFn() {
        st.stopAllShards({}, true /* forRestart */);
        st.stopAllConfigServers({}, true /* forRestart */);
        st.restartAllConfigServers();
        st.restartAllShards();
        return st.s;
    }

    function configureFailPointOnConfigsvrFn(failPointName, mode) {
        assert.commandWorked(
            st.configRS.getPrimary().getDB("admin").runCommand({configureFailPoint: failPointName, mode: mode}),
        );
    }

    function configureFailPointOnShard1Fn(failPointName, mode) {
        assert.commandWorked(
            st.rs1.getPrimary().getDB("admin").runCommand({configureFailPoint: failPointName, mode: mode}),
        );
    }

    jsTestLog("Running sharded cluster tests.");

    jsTestLog("Test when the configsvr is the one failing during cleanup.");
    runTest(st.s, restartDeploymentFn, configureFailPointOnConfigsvrFn);

    jsTestLog("Test when a shard is the one failing during cleanup.");
    runTest(st.s, restartDeploymentFn, configureFailPointOnShard1Fn);

    jsTestLog("Sharded cluster tests completed successfully");

    st.stop();
}
runStandaloneTest();
runReplicaSetTest();
runShardedClusterTest();
