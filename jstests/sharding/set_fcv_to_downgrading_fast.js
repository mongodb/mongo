/**
 * Tests that FCV downgrade will reach the transitional kDowngrading state quickly (within a few
 * seconds).
 *
 * @tags: [featureFlagDowngradingToUpgrading, multiversion_incompatible, does_not_support_stepdowns]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/feature_flag_util.js");

const latest = "latest";
// The FCV downgrade should be < 1 second but we added a buffer for slow machines.
const timeoutSeconds = 3;

function runStandaloneTest() {
    jsTestLog("Running standalone test");
    const conn = MongoRunner.runMongod({binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    const adminDB = conn.getDB("admin");

    if (!FeatureFlagUtil.isEnabled(adminDB, "DowngradingToUpgrading")) {
        jsTestLog("Skipping as featureFlagDowngradingToUpgrading is not enabled");
        MongoRunner.stopMongod(conn);
        return;
    }

    const fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("current FCV (should be latest): " + tojson(fcvDoc));
    checkFCV(adminDB, latestFCV);

    assert.commandWorked(
        conn.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));

    const parallelShell = startParallelShell(function() {
        db.getSiblingDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV});
    }, conn.port);

    // Check that we reach the downgrading FCV state within a few seconds.
    assert.soon(
        () => {
            return isFCVEqual(adminDB, lastLTSFCV, lastLTSFCV);
        },
        "standalone FCV failed to reach downgrading state within " + timeoutSeconds + " seconds",
        timeoutSeconds * 1000);
    parallelShell();

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    jsTestLog("Running replica set test");
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

    const fcvDoc = primaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("current FCV (should be latest): " + tojson(fcvDoc));
    checkFCV(primaryAdminDB, latestFCV);

    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));

    const parallelShell = startParallelShell(function() {
        db.getSiblingDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV});
    }, primary.port);

    // Check that we reach the downgrading FCV state within a few seconds.
    assert.soon(
        () => {
            return isFCVEqual(primaryAdminDB, lastLTSFCV, lastLTSFCV);
        },
        "replica set FCV failed to reach downgrading state within " + timeoutSeconds + " seconds",
        timeoutSeconds * 1000);
    parallelShell();

    rst.stopSet();
}

function runShardingTest() {
    jsTestLog("Running sharding test");
    const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
    const mongosAdminDB = st.s.getDB("admin");
    const configPrimary = st.configRS.getPrimary();
    const configPrimaryAdminDB = configPrimary.getDB("admin");
    const shard0Primary = st.rs0.getPrimary();
    const shard0PrimaryAdminDB = shard0Primary.getDB("admin");
    const shard1Primary = st.rs1.getPrimary();
    const shard1PrimaryAdminDB = shard1Primary.getDB("admin");

    // Make sure all servers start as latest version.
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(mongosAdminDB, latestFCV);
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shard0PrimaryAdminDB, latestFCV);
    checkFCV(shard1PrimaryAdminDB, latestFCV);

    assert.commandWorked(
        shard0Primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandWorked(
        shard1Primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));

    const parallelShell = startParallelShell(function() {
        db.getSiblingDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV});
    }, st.s.port);

    // Check that we reach the downgrading FCV state within a few seconds.
    assert.soon(
        () => {
            const configFCVEqual = isFCVEqual(configPrimaryAdminDB, lastLTSFCV, lastLTSFCV);
            jsTestLog("configFCVEqual: " + tojson(configFCVEqual));
            const shard0FCVEqual = isFCVEqual(shard0PrimaryAdminDB, lastLTSFCV, lastLTSFCV);
            jsTestLog("shard0FCVEqual: " + tojson(shard0FCVEqual));
            const shard1FCVEqual = isFCVEqual(shard1PrimaryAdminDB, lastLTSFCV, lastLTSFCV);
            jsTestLog("shard1FCVEqual: " + tojson(shard1FCVEqual));

            const mongosFCVEqual = isFCVEqual(mongosAdminDB, lastLTSFCV, lastLTSFCV);
            jsTestLog("mongosFCVEqual: " + tojson(mongosFCVEqual));

            return configFCVEqual && shard0FCVEqual && shard1FCVEqual && mongosFCVEqual;
        },
        "sharded cluster FCV failed to reach downgrading state within " + timeoutSeconds +
            " seconds",
        timeoutSeconds * 1000);

    parallelShell();
    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTest();
})();
