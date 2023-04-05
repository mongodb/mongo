/**
 * This file tests that when a cluster is in downgrading FCV stage, after we restart the cluster,
 * FCV is still in downgrading state and we can change FCV to upgraded state.
 */

(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

const latest = "latest";
const testName = "restart_during_downgrading_fcv";
const dbpath = MongoRunner.dataPath + testName;

const setup = function(conn, configPrimary) {
    const adminDB = conn.getDB("admin");
    if (configPrimary) {
        assert.commandWorked(
            configPrimary.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));
    } else {
        assert.commandWorked(
            conn.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));
    }

    assert.commandFailed(conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    // Check FCV is in downgrading state.
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV);
};

const runTest = function(conn) {
    const adminDB = conn.getDB("admin");
    // Check FCV is still in downgrading state.
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV);
    jsTestLog("Set FCV to upgrade");
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);
};

const runStandaloneTest = function() {
    jsTestLog("Starting standalone test");
    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});

    setup(conn);
    jsTestLog("Restarting mongod");
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    jsTestLog("Mongod is restarted");
    runTest(conn);

    MongoRunner.stopMongod(conn);
};

const runReplicaSetTest = function() {
    jsTestLog("Starting replica set test");
    const rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    let primary = rst.getPrimary();

    setup(primary);
    jsTestLog("Restarting replica set");
    rst.stopSet(null /* signal */, true /* forRestart */);
    rst.startSet({restart: true});
    primary = rst.getPrimary();
    jsTestLog("Replica set is restarted");
    runTest(primary);

    rst.stopSet();
};

const runShardedClusterTest = function() {
    jsTestLog("Starting sharded cluster test");
    const st = new ShardingTest({
        shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}, {binVersion: latest}]}}
    });
    let mongos = st.s;
    const configPrimary = st.configRS.getPrimary();

    setup(mongos, configPrimary);
    jsTestLog("Restarting sharded cluster");
    st.stopAllShards({}, true /* forRestart */);
    st.stopAllConfigServers({}, true /* forRestart */);
    st.restartAllConfigServers();
    st.restartAllShards();
    mongos = st.s;
    jsTestLog("Sharded cluster is restarted");
    runTest(mongos);

    st.stop();
};

runStandaloneTest();
runReplicaSetTest();
runShardedClusterTest();
})();
