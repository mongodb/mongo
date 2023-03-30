/**
 * Test that FCV upgrade fails if downgrading fails during isCleaningServerMetadata phase.
 *
 * @tags: [requires_fcv_70]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/feature_flag_util.js");

const latest = "latest";
const testName = "fcv_upgrade_fails_during_is_cleaning_server_metadata";
const dbpath = MongoRunner.dataPath + testName;

function upgradeFailsDuringIsCleaningServerMetadata(
    conn, restartDeploymentFn, configureFailPointFn) {
    let adminDB = conn.getDB("admin");

    configureFailPointFn('failDowngradingDuringIsCleaningServerMetadata', 'alwaysOn');

    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    let fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("Current FCV should be in isCleaningServerMetadata phase: " + tojson(fcvDoc));
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV, true /* isCleaningServerMetadata */);

    // Upgrade should fail because we are in isCleaningServerMetadata.
    assert.commandFailedWithCode(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}),
                                 7428200);

    configureFailPointFn('failDowngradingDuringIsCleaningServerMetadata', 'off');

    // We are still in isCleaningServerMetadata even if we retry and fail downgrade at an earlier
    // point.
    jsTestLog(
        "Test that retrying downgrade and failing at an earlier point will still keep failing upgrade");
    configureFailPointFn('failDowngrading', 'alwaysOn');
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("Current FCV should still be in isCleaningServerMetadata phase: " + tojson(fcvDoc));
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV, true /* isCleaningServerMetadata */);

    // Upgrade should fail because we are in isCleaningServerMetadata.
    assert.commandFailedWithCode(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}),
                                 7428200);

    jsTestLog("isCleaningServerMetadata should persist through restarts.");
    conn = restartDeploymentFn();
    adminDB = conn.getDB("admin");

    fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("Current FCV should still be in isCleaningServerMetadata phase: " + tojson(fcvDoc));
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV, true /* isCleaningServerMetadata */);

    // Upgrade should still fail because we are in isCleaningServerMetadata.
    assert.commandFailedWithCode(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}),
                                 7428200);

    // Completing downgrade and then upgrading succeeds.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(adminDB, lastLTSFCV);

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);
}

function runStandaloneTest() {
    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    const adminDB = conn.getDB("admin");

    function restartDeploymentFn() {
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
        assert.neq(
            null,
            conn,
            "mongod was unable to start up with version=" + latest + " and existing data files");
        return conn;
    }

    function configureFailPointFn(failPointName, mode) {
        assert.commandWorked(
            conn.getDB("admin").runCommand({configureFailPoint: failPointName, mode: mode}));
    }

    upgradeFailsDuringIsCleaningServerMetadata(conn, restartDeploymentFn, configureFailPointFn);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    const rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    const primaryAdminDB = rst.getPrimary().getDB("admin");
    const primary = rst.getPrimary();

    function restartDeploymentFn() {
        rst.stopSet(null /* signal */, true /* forRestart */);
        rst.startSet({restart: true});
        return rst.getPrimary();
    }

    function configureFailPointFn(failPointName, mode) {
        assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
            {configureFailPoint: failPointName, mode: mode}));
    }

    upgradeFailsDuringIsCleaningServerMetadata(primary, restartDeploymentFn, configureFailPointFn);
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
        assert.commandWorked(st.configRS.getPrimary().getDB("admin").runCommand(
            {configureFailPoint: failPointName, mode: mode}));
    }

    function configureFailPointOnShard1Fn(failPointName, mode) {
        assert.commandWorked(st.rs1.getPrimary().getDB("admin").runCommand(
            {configureFailPoint: failPointName, mode: mode}));
    }

    // Test when the configsvr is the one that fails during the cleanup metadata phase.
    upgradeFailsDuringIsCleaningServerMetadata(
        st.s, restartDeploymentFn, configureFailPointOnConfigsvrFn);

    // Test when a shard is the one that fails during the cleanup metadata phase.
    upgradeFailsDuringIsCleaningServerMetadata(
        st.s, restartDeploymentFn, configureFailPointOnShard1Fn);

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardedClusterTest();
})();
