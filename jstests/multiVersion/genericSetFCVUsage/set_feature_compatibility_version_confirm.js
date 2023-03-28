/**
 * Tests the 'confirm: true' parameter in setFeatureCompatibilityVersion.
 */

(function() {
"use strict";

TestData.setParameters = TestData.setParameters || {};
TestData.setParameters.requireConfirmInSetFcv = true;

let dbpath = MongoRunner.dataPath + "feature_compatibility_version_confirm";
resetDbpath(dbpath);

const latest = "latest";

function runTest(conn, downgradeVersion, configSvrPrimary = null) {
    const downgradeFCV = binVersionToFCV(downgradeVersion);

    const checkFCVConn = configSvrPrimary !== null ? configSvrPrimary : conn;
    checkFCV(checkFCVConn, latestFCV);

    // Fail when downgrading the node if 'confirm: true' is not specified.
    assert.commandFailedWithCode(conn.runCommand({setFeatureCompatibilityVersion: downgradeFCV}),
                                 7369100);
    checkFCV(checkFCVConn, latestFCV);

    // Successfully downgrade if 'confirm: true' is specified.
    assert.commandWorked(
        conn.runCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));
    checkFCV(checkFCVConn, downgradeFCV);

    // Fail when upgrading the node if 'confirm: true' is not specified.
    assert.commandFailedWithCode(conn.runCommand({setFeatureCompatibilityVersion: latestFCV}),
                                 7369100);
    checkFCV(checkFCVConn, downgradeFCV);

    // Successfully upgrade if 'confirm: true' is specified.
    assert.commandWorked(
        conn.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    checkFCV(checkFCVConn, latestFCV);
}

function runStandaloneTest(downgradeVersion) {
    jsTestLog("Running standalone downgrade test");

    // Spin up a standalone with latest FCV.
    const conn = MongoRunner.runMongod(
        {dbpath: dbpath, binVersion: latest, setParameter: {requireConfirmInSetFcv: true}});
    assert.neq(null, conn, "mongod was unable to start up");
    const adminDB = conn.getDB("admin");
    runTest(adminDB, downgradeVersion);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest(downgradeVersion) {
    jsTestLog("Running replica set downgrade test");

    // Spin up a replica set with latest FCV.
    const rst = new ReplSetTest({
        nodes: 3,
        nodeOptions: {binVersion: latest, setParameter: {requireConfirmInSetFcv: true}}
    });
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    const primary = rst.getPrimary();
    const adminDB = primary.getDB("admin");
    runTest(adminDB, downgradeVersion);

    rst.stopSet();
}

function runShardingTest(downgradeVersion) {
    jsTestLog("Running sharded cluster downgrade test");

    // Spin up a sharded cluster with latest FCV.
    const st =
        new ShardingTest({shards: 2, rsOptions: {setParameter: {requireConfirmInSetFcv: true}}});
    const conn = st.s;
    const adminDB = conn.getDB("admin");
    const configSvrPrimaryConn = st.configRS.getPrimary().getDB("admin");

    runTest(adminDB, downgradeVersion, configSvrPrimaryConn);

    st.stop();
}

runStandaloneTest('last-lts');
runReplicaSetTest('last-lts');
runShardingTest('last-lts');

if (lastLTSFCV != lastContinuousFCV) {
    runStandaloneTest('last-continuous');
    runReplicaSetTest('last-continuous');
    runShardingTest('last-continuous');
}

TestData.setParameters.requireConfirmInSetFcv = false;
})();
