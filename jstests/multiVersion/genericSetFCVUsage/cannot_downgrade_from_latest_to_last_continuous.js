/**
 * Tests that we cannot downgrade from latest to last continuous.
 */

(function() {
"use strict";

TestData.setParameters = TestData.setParameters || {};
TestData.setParameters.disableTransitionFromLatestToLastContinuous = true;

let dbpath = MongoRunner.dataPath + "cannot_downgrade_from_latest_to_last_continuous";
resetDbpath(dbpath);

function runTest(conn, configSvrPrimary = null) {
    const checkFCVConn = configSvrPrimary !== null ? configSvrPrimary : conn;
    checkFCV(checkFCVConn, latestFCV);

    // Fail when attempting to transition from latest to last continuous.
    assert.commandFailedWithCode(
        conn.runCommand({setFeatureCompatibilityVersion: lastContinuousFCV}), 5147403);
    checkFCV(checkFCVConn, latestFCV);

    // Successfully downgrade to last LTS FCV.
    assert.commandWorked(conn.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    checkFCV(checkFCVConn, lastLTSFCV);
}

function runStandaloneTest() {
    jsTestLog("Running standalone downgrade test");

    // Spin up a standalone with latest FCV.
    const conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
    assert.neq(null, conn, "mongod was unable to start up");
    const adminDB = conn.getDB("admin");
    runTest(adminDB);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    jsTestLog("Running replica set downgrade test");

    // Spin up a replica set with latest FCV.
    const rst = new ReplSetTest({nodes: 3, nodeOptions: {binVersion: "latest"}});
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    const primary = rst.getPrimary();
    const adminDB = primary.getDB("admin");
    runTest(adminDB);

    rst.stopSet();
}

function runShardingTest() {
    jsTestLog("Running sharded cluster downgrade test");

    // Spin up a sharded cluster with latest FCV.
    const st = new ShardingTest({shards: 2});
    const conn = st.s;
    const adminDB = conn.getDB("admin");
    const configSvrPrimaryConn = st.configRS.getPrimary().getDB("admin");

    runTest(adminDB, configSvrPrimaryConn);

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTest();

TestData.setParameters.disableTransitionFromLatestToLastContinuous = false;
})();
