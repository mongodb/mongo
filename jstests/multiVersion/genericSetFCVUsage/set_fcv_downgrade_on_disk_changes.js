/**
 * Unified test that makes sure calling setFeatureCompatibilityVersion with
 * {downgradeOnDiskChanges: true} is able to successfully downgrade all expected on-disk changes in
 * one invocation.
 */
(function() {
"use strict";

load('jstests/multiVersion/libs/multi_rs.js');
load('jstests/multiVersion/libs/multi_cluster.js');

let dbpath = MongoRunner.dataPath + jsTestName();
resetDbpath(dbpath);

const latest = "latest";
const lastContinuous = "last-continuous";

/**
 * Each new feature that adds downgrade logic to the setFeatureCompatibilityVersion command with
 * {downgradeOnDiskChanges: true} should add their test cases to this test file. Each test case
 * should follow this 'dummyTest' template and implement the following three test functions:
 * this.onDiskChangesBeforeDowngrade: This function will be called before the FCV downgrade to
 * introduce durable on-disk changes that will be downgraded as part the FCV downgrade.
 * this.validateAfterFCVDowngrade: This function will be called after the FCV downgrade to validate
 * the downgrade of the incompatible on-disk changes introduced in onDiskChangesBeforeDowngrade.
 * this.validateAfterBinaryDowngrade: This function will be called after the binary downgrade to
 * 'last-continuous' binaries to validate the downgrade of the incompatible on-disk changes
 * introduced in onDiskChangesBeforeDowngrade.
 *
 * Each new test should also be added to the downgradeOnDiskChangesTests list below.
 */
function dummyTest() {
    const documents = [{_id: "dummy"}];
    this.onDiskChangesBeforeDowngrade = function(conn) {
        jsTestLog("Running onDiskChangesBeforeDowngrade of dummyTest");
        let testDB = conn.getDB("test");
        assert.commandWorked(testDB.runCommand({insert: "dummy", documents: documents}));
    };

    this.validateAfterFCVDowngrade = function(conn) {
        jsTestLog("Running validateAfterFCVDowngrade of dummyTest");
        let testDB = conn.getDB("test");
        let res = testDB.dummy.find({});
        assert.sameMembers(res.toArray(), documents, () => tojson(res));
    };

    this.validateAfterBinaryDowngrade = function(conn) {
        jsTestLog("Running validateAfterBinaryDowngrade of dummyTest");
        this.validateAfterFCVDowngrade(conn);
    };
}

const downgradeOnDiskChangesTests = [
    new dummyTest(),
];

function runStandaloneTest() {
    jsTestLog("Running standalone test");

    let conn;
    let adminDB;

    // A 'latest' binary standalone should default to 'latestFCV'.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);

    jsTestLog("Introducing on-disk changes to be downgraded in FCV downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.onDiskChangesBeforeDowngrade(conn);
    }

    jsTestLog(
        "Test that setFeatureCompatibilityVersion succeeds with {downgradeOnDiskChanges: true}");
    assert.commandWorked(adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, downgradeOnDiskChanges: true}));
    checkFCV(adminDB, lastContinuousFCV);
    checkLog.contains(conn, "Downgrading on-disk format");

    jsTestLog("Validating on-disk changes after FCV downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.validateAfterFCVDowngrade(conn);
    }

    MongoRunner.stopMongod(conn);

    // Test that the node can restart with a last-continuous binary.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastContinuous, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with binary version=" + lastContinuous +
                   " and featureCompatibilityVersion=" + lastContinuousFCV);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastContinuousFCV);

    jsTestLog("Validating on-disk changes after binary downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.validateAfterBinaryDowngrade(conn);
    }

    MongoRunner.stopMongod(conn);

    // Test that the node can restart with a latest binary.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latest, noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with binary version=" + latest +
                   " and featureCompatibilityVersion=" + lastContinuousFCV);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastContinuousFCV);

    // Test that the FCV can be upgraded back to 'latestFCV'.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    jsTestLog("Running replica set test");

    // 'latest' binary replica set.
    let rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiateWithHighElectionTimeout();
    let primaryAdminDB = rst.getPrimary().getDB("admin");
    let secondaryAdminDB = rst.getSecondary().getDB("admin");

    // FCV should default to 'latestFCV' on primary and secondary in a 'latest' binary replica set.
    checkFCV(primaryAdminDB, latestFCV);
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, latestFCV);

    jsTestLog("Introducing on-disk changes to be downgraded in FCV downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.onDiskChangesBeforeDowngrade(rst.getPrimary());
    }

    jsTestLog(
        "Test that setFeatureCompatibilityVersion succeeds with {downgradeOnDiskChanges: true} " +
        "and propogates to the secondary");
    assert.commandWorked(primaryAdminDB.runCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, downgradeOnDiskChanges: true}));
    checkFCV(primaryAdminDB, lastContinuousFCV);
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, lastContinuousFCV);

    jsTestLog("Validating on-disk changes after FCV downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.validateAfterFCVDowngrade(rst.getPrimary());
    }

    // Test that the cluster can restart with a last-continuous binary.
    rst.upgradeSet({binVersion: lastContinuous});
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(primaryAdminDB, lastContinuousFCV);
    checkFCV(secondaryAdminDB, lastContinuousFCV);

    jsTestLog("Validating on-disk changes after binary downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.validateAfterBinaryDowngrade(rst.getPrimary());
    }

    // Test that the cluster can restart with a latest binary.
    rst.upgradeSet({binVersion: latest});
    primaryAdminDB = rst.getPrimary().getDB("admin");
    secondaryAdminDB = rst.getSecondary().getDB("admin");
    checkFCV(primaryAdminDB, lastContinuousFCV);
    checkFCV(secondaryAdminDB, lastContinuousFCV);

    // Test that the FCV can be upgraded back to 'latestFCV'.
    assert.commandWorked(primaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(primaryAdminDB, latestFCV);
    rst.awaitReplication();
    checkFCV(secondaryAdminDB, latestFCV);

    rst.stopSet();
}

function runShardingTest() {
    jsTestLog("Running sharding test");

    // A 'latest' binary cluster started with clean data files will set FCV to 'latestFCV'.
    let st =
        new ShardingTest({shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}}});
    let mongosAdminDB = st.s.getDB("admin");
    let configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    let shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");

    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    jsTestLog("Introducing on-disk changes to be downgraded in FCV downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.onDiskChangesBeforeDowngrade(st.s);
    }

    jsTestLog(
        "Test that setFeatureCompatibilityVersion succeeds with {downgradeOnDiskChanges: true} " +
        "on mongos");
    assert.commandWorked(mongosAdminDB.runCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, downgradeOnDiskChanges: true}));

    // FCV propagates to config and shard.
    checkFCV(configPrimaryAdminDB, lastContinuousFCV);
    checkFCV(shardPrimaryAdminDB, lastContinuousFCV);

    jsTestLog("Validating on-disk changes after FCV downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.validateAfterFCVDowngrade(st.s);
    }

    // Test that the cluster can restart with a last-continuous binary.
    st.upgradeCluster(lastContinuous, {waitUntilStable: true});
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, lastContinuousFCV);
    checkFCV(shardPrimaryAdminDB, lastContinuousFCV);

    jsTestLog("Validating on-disk changes after binary downgrade");
    for (let test of downgradeOnDiskChangesTests) {
        test.validateAfterBinaryDowngrade(st.s);
    }

    // Test that the cluster can restart with a latest binary.
    st.upgradeCluster(latest, {waitUntilStable: true});
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, lastContinuousFCV);
    checkFCV(shardPrimaryAdminDB, lastContinuousFCV);

    // Test that the FCV can be upgraded back to 'latestFCV'.
    mongosAdminDB = st.s.getDB("admin");
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTest();
})();
