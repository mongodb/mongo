/*
 * Tests startup with a node in downgrading state.
 * Starts a sharded cluster with 2 shards, each with 2 nodes.
 *
 *  @tags: [featureFlagDowngradingToUpgrading]
 */

(function() {
"use strict";

load('jstests/multiVersion/libs/verify_versions.js');
load('jstests/libs/fail_point_util.js');
load("jstests/libs/feature_flag_util.js");

function runSharding() {
    let fcvDoc;
    let shard0PrimaryAdminDB;
    let shard1PrimaryAdminDB;
    let mongosAdminDB;
    let configPrimary;
    let configPrimaryAdminDB;

    const st = new ShardingTest({shards: {rs0: {nodes: 2}, rs1: {nodes: 2}}, config: 1, mongos: 1});

    mongosAdminDB = st.s.getDB("admin");
    configPrimary = st.configRS.getPrimary();
    configPrimaryAdminDB = configPrimary.getDB("admin");
    shard0PrimaryAdminDB = st.shard0.getDB("admin");
    shard1PrimaryAdminDB = st.shard1.getDB("admin");

    // Check that the shards are using latest version.
    fcvDoc = shard0PrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Shard0 version before downgrading: ${tojson(fcvDoc)}`);
    checkFCV(shard0PrimaryAdminDB, latestFCV);

    fcvDoc = shard1PrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Shard1 version before downgrading: ${tojson(fcvDoc)}`);
    checkFCV(shard1PrimaryAdminDB, latestFCV);

    // Set the failDowngrading failpoint so that the downgrading will fail.
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));

    // Start downgrading. It will fail.
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    st.rs0.awaitReplication();
    st.rs1.awaitReplication();

    // Check that the shards are in downgrading state.
    fcvDoc = configPrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Config version after downgrading: ${tojson(fcvDoc)}`);
    checkFCV(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    fcvDoc = shard0PrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Shard0 version after downgrading: ${tojson(fcvDoc)}`);
    checkFCV(shard0PrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    fcvDoc = shard1PrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Shard1 version after downgrading: ${tojson(fcvDoc)}`);
    checkFCV(shard1PrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    // Restart the sharded cluster.
    jsTestLog("Restarting the config server:");
    st.restartConfigServer(0);
    jsTestLog("Restarting the mongos:");
    st.restartMongos(0);
    jsTestLog("Restarting shard0:");
    st.restartShardRS(0, {startClean: false}, undefined, true);
    jsTestLog("Restarting shard1:");
    st.restartShardRS(1, {startClean: false}, undefined, true);

    st.waitForShardingInitialized();

    // Check that the shards are in downgrading state after restarting.
    configPrimary = st.configRS.getPrimary();
    configPrimaryAdminDB = configPrimary.getDB("admin");
    fcvDoc = configPrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Config version after restarting: ${tojson(fcvDoc)}`);
    checkFCV(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    shard0PrimaryAdminDB = st.shard0.getDB('admin');
    fcvDoc = shard0PrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Shard0 version after restarting: ${tojson(fcvDoc)}`);
    checkFCV(shard0PrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    shard1PrimaryAdminDB = st.shard1.getDB('admin');
    fcvDoc = shard1PrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Shard1 version after restarting: ${tojson(fcvDoc)}`);
    checkFCV(shard1PrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    // Upgrade the sharded cluster to upgraded (latestFCV).
    mongosAdminDB = st.s.getDB("admin");
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));

    st.rs0.awaitReplication();
    st.rs1.awaitReplication();

    // Check that the shards are in upgraded state.
    fcvDoc = configPrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Config version after successfully upgrading: ${tojson(fcvDoc)}`);
    checkFCV(configPrimaryAdminDB, latestFCV);

    shard0PrimaryAdminDB = st.shard0.getDB('admin');
    fcvDoc = shard0PrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Shard0 version after successfully upgrading: ${tojson(fcvDoc)}`);
    checkFCV(shard0PrimaryAdminDB, latestFCV);

    shard1PrimaryAdminDB = st.shard1.getDB('admin');
    fcvDoc = shard1PrimaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Shard1 version after successfully upgrading: ${tojson(fcvDoc)}`);
    checkFCV(shard1PrimaryAdminDB, latestFCV);

    st.stop();
}

runSharding();
})();
