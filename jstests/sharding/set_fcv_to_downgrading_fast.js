/**
 * Tests that FCV downgrade will reach the transitional kDowngrading state quickly (within a few
 * seconds).
 *
 * Config shard incompatible because we do not currently allow downgrading FCV with a catalog
 * shard. TODO SERVER-73279: Enable in config shard mode when it supports FCV downgrade.
 * @tags: [
 *   requires_fcv_70,
 *   multiversion_incompatible,
 *   does_not_support_stepdowns,
 *   config_shard_incompatible,
 * ]
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

    const fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("current FCV (should be latest): " + tojson(fcvDoc));
    checkFCV(adminDB, latestFCV);

    const hangAtSetFCVStartFailpoint = configureFailPoint(conn, "hangAtSetFCVStart");
    assert.commandWorked(conn.adminCommand(
        {configureFailPoint: 'failAfterReachingTransitioningState', mode: "alwaysOn"}));

    const parallelShell = startParallelShell(function() {
        db.getSiblingDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV});
    }, conn.port);

    // Make sure the setFCV command has started running.
    hangAtSetFCVStartFailpoint.wait();
    hangAtSetFCVStartFailpoint.off();

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

    const fcvDoc = primaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("current FCV (should be latest): " + tojson(fcvDoc));
    checkFCV(primaryAdminDB, latestFCV);

    const hangAtSetFCVStartFailpoint = configureFailPoint(primary, "hangAtSetFCVStart");
    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: 'failAfterReachingTransitioningState', mode: "alwaysOn"}));

    const parallelShell = startParallelShell(function() {
        db.getSiblingDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV});
    }, primary.port);

    // Make sure the setFCV command has started running.
    hangAtSetFCVStartFailpoint.wait();
    hangAtSetFCVStartFailpoint.off();

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

    const hangAtSetFCVStartFailpoint = configureFailPoint(configPrimary, "hangAtSetFCVStart");
    assert.commandWorked(configPrimary.adminCommand(
        {configureFailPoint: 'failAfterSendingShardsToDowngradingOrUpgrading', mode: "alwaysOn"}));

    const parallelShell = startParallelShell(function() {
        db.getSiblingDB("admin").runCommand({setFeatureCompatibilityVersion: lastLTSFCV});
    }, st.s.port);

    // Make sure the setFCV command has started running.
    hangAtSetFCVStartFailpoint.wait();
    hangAtSetFCVStartFailpoint.off();

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
