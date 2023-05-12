/**
 * Tests the defaultStartupFCV startup parameter.
 */

(function() {
"use strict";

TestData.setParameters = TestData.setParameters || {};
TestData.setParameters.disableTransitionFromLatestToLastContinuous = true;

const latest = "latest";
const testName = "default_startup_FCV_parameter";
const dbpath = MongoRunner.dataPath + testName;
resetDbpath(dbpath);

function runStandaloneTest() {
    jsTestLog("Test starting with defaultStartupFCV = lastLTS");
    let conn = MongoRunner.runMongod(
        {binVersion: latest, setParameter: "defaultStartupFCV=" + lastLTSFCV});
    assert.neq(null, conn);
    let adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastLTSFCV);
    MongoRunner.stopMongod(conn);

    jsTestLog("Test starting with defaultStartupFCV = lastContinuous");
    conn = MongoRunner.runMongod({
        binVersion: latest,
        dbpath: dbpath,
        setParameter: "defaultStartupFCV=" + lastContinuousFCV
    });
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastContinuousFCV);
    MongoRunner.stopMongod(conn);

    clearRawMongoProgramOutput();
    jsTestLog("Test starting with defaultStartupFCV when there is already an existing FCV.");
    conn = MongoRunner.runMongod({
        binVersion: latest,
        dbpath: dbpath,
        noCleanData: true,
        setParameter: "defaultStartupFCV=" + lastLTSFCV
    });
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");
    // The FCV should still be the original FCV, not the provided defaultStartupFCV.
    checkFCV(adminDB, lastContinuousFCV);
    assert(rawMongoProgramOutput().includes(
        "Ignoring the provided defaultStartupFCV parameter since the FCV already exists"));
    MongoRunner.stopMongod(conn);

    jsTestLog("Test starting with defaultStartupFCV = latest");
    conn =
        MongoRunner.runMongod({binVersion: latest, setParameter: "defaultStartupFCV=" + latestFCV});
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);
    MongoRunner.stopMongod(conn);

    clearRawMongoProgramOutput();
    jsTestLog("Test starting with invalid defaultStartupFCV, FCV should default to latest");
    conn = MongoRunner.runMongod({binVersion: latest, setParameter: "defaultStartupFCV=hello"});
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);
    assert(rawMongoProgramOutput().includes("The provided 'defaultStartupFCV' is not a valid FCV"));
    MongoRunner.stopMongod(conn);

    clearRawMongoProgramOutput();
    jsTestLog("Test starting with invalid defaultStartupFCV, FCV should default to latest");
    conn = MongoRunner.runMongod({binVersion: latest, setParameter: "defaultStartupFCV=5.0"});
    assert.neq(null, conn);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);
    assert(rawMongoProgramOutput().includes("The provided 'defaultStartupFCV' is not a valid FCV"));
    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    jsTestLog("Test starting with defaultStartupFCV = lastLTS");
    let rst = new ReplSetTest({
        nodes: [
            {
                binVersion: latest,
                setParameter: {defaultStartupFCV: lastLTSFCV},

            },
            {
                binVersion: latest,
                // The second node will initial sync from the primary and end up with lastLTSFCV.
                setParameter: {defaultStartupFCV: lastContinuousFCV},
                rsConfig: {priority: 0},
            }
        ]
    });
    rst.startSet();
    rst.initiate();
    assert.neq(null, rst);
    let primaryAdminDB = rst.getPrimary().getDB("admin");
    let secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(primaryAdminDB, lastLTSFCV);
    checkFCV(secondaryAdminDB, lastLTSFCV);
    rst.stopSet();

    jsTestLog("Test starting with defaultStartupFCV = lastContinuous");
    rst = new ReplSetTest({
        nodes: [
            {
                binVersion: latest,
                dbpath: dbpath + "1",
                setParameter: {defaultStartupFCV: lastContinuousFCV},

            },
            {
                binVersion: latest,
                dbpath: dbpath + "2",
                // The second node will initial sync from the primary and end up with
                // lastContinuousFCV.
                setParameter: {defaultStartupFCV: lastLTSFCV},
                rsConfig: {priority: 0},
            }
        ]
    });
    rst.startSet();
    rst.initiate();
    assert.neq(null, rst);
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(primaryAdminDB, lastContinuousFCV);
    checkFCV(secondaryAdminDB, lastContinuousFCV);
    rst.stopSet(null /* signal */, true /* forRestart */);

    clearRawMongoProgramOutput();
    jsTestLog("Test starting with defaultStartupFCV when there is already an existing FCV.");
    rst.startSet({restart: true, setParameter: {defaultStartupFCV: lastLTSFCV}});
    assert.neq(null, rst);
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    // The FCV should still be the original FCV, not the provided defaultStartupFCV.
    checkFCV(primaryAdminDB, lastContinuousFCV);
    checkFCV(secondaryAdminDB, lastContinuousFCV);
    rst.stopSet();

    jsTestLog("Test starting with defaultStartupFCV = latest");
    rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: latest, setParameter: {defaultStartupFCV: latestFCV}}
    });
    rst.startSet();
    rst.initiate();
    assert.neq(null, rst);
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(primaryAdminDB, latestFCV);
    checkFCV(secondaryAdminDB, latestFCV);
    rst.stopSet();

    clearRawMongoProgramOutput();
    jsTestLog("Test starting with invalid defaultStartupFCV, FCV should default to latest");
    rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {binVersion: latest, setParameter: {defaultStartupFCV: "hello"}}});
    rst.startSet();
    rst.initiate();
    assert.neq(null, rst);
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(primaryAdminDB, latestFCV);
    checkFCV(secondaryAdminDB, latestFCV);
    assert(rawMongoProgramOutput().includes("The provided 'defaultStartupFCV' is not a valid FCV"));
    rst.stopSet();

    clearRawMongoProgramOutput();
    jsTestLog("Test starting with invalid defaultStartupFCV, FCV should default to latest");
    rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {binVersion: latest, setParameter: {defaultStartupFCV: "5.0"}}});
    rst.startSet();
    rst.initiate();
    assert.neq(null, rst);
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(primaryAdminDB, latestFCV);
    checkFCV(secondaryAdminDB, latestFCV);
    assert(rawMongoProgramOutput().includes("The provided 'defaultStartupFCV' is not a valid FCV"));
    rst.stopSet();
}

function runShardingTest() {
    jsTestLog("Test starting sharded cluster with defaultStartupFCV = lastLTS");
    let st = new ShardingTest({
        shards: 2,
        mongos: 1,
        config: 1,
        // Shards should ignore the defaultStartupFCV parameter.
        shardOptions: {binVersion: latest, setParameter: {defaultStartupFCV: latestFCV}},
        configOptions: {binVersion: latest, setParameter: {defaultStartupFCV: lastLTSFCV}}
    });
    let configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    let shard0PrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    let shard1PrimaryAdminDB = st.rs1.getPrimary().getDB("admin");

    checkFCV(configPrimaryAdminDB, lastLTSFCV);
    checkFCV(shard0PrimaryAdminDB, lastLTSFCV);
    checkFCV(shard1PrimaryAdminDB, lastLTSFCV);
    st.stop();

    st = new ShardingTest({
        shards: 2,
        mongos: 1,
        config: 1,
        configOptions: {binVersion: latest, setParameter: {defaultStartupFCV: lastContinuousFCV}}
    });
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shard0PrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    shard1PrimaryAdminDB = st.rs1.getPrimary().getDB("admin");

    checkFCV(configPrimaryAdminDB, lastContinuousFCV);
    checkFCV(shard0PrimaryAdminDB, lastContinuousFCV);
    checkFCV(shard1PrimaryAdminDB, lastContinuousFCV);

    jsTestLog("Test that a replica set started with shardsvr still defaults to lastLTS");
    const newShard = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: latest, setParameter: {defaultStartupFCV: latestFCV}}
    });
    newShard.startSet({shardsvr: ''});
    newShard.initiate();

    let primaryAdminDB = newShard.getPrimary().getDB("admin");
    let secondaryAdminDB = newShard.getSecondary().getDB("admin");
    checkFCV(primaryAdminDB, lastLTSFCV);
    checkFCV(secondaryAdminDB, lastLTSFCV);
    assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShard.name}));

    jsTestLog("Test that the FCV should be set to the cluster's FCV after running addShard");
    checkFCV(primaryAdminDB, lastContinuousFCV);
    checkFCV(secondaryAdminDB, lastContinuousFCV);
    newShard.stopSet();
    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTest();
})();
