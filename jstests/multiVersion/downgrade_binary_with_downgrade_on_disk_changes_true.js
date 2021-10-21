/**
 * Tests that downgrading a clean cluster with no data from latest to last-continuous binary version
 * with downgradeOnDiskChanges:true succeeds.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/multiVersion/libs/multi_cluster.js');

function testStandaloneDowngrade() {
    const dbpath = MongoRunner.dataPath + jsTestName();
    resetDbpath(dbpath);
    let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
    assert.neq(null, conn, "mongod was unable to start up with version=latest");
    let adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);

    jsTestLog("Downgrading FCV to last-continuous with {downgradeOnDiskChanges: true}");
    assert.commandWorked(adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, downgradeOnDiskChanges: true}));
    checkFCV(adminDB, lastContinuousFCV);
    checkLog.contains(conn, "Downgrading on-disk format");

    jsTestLog("Downgrading binary to last-continuous");
    MongoRunner.stopMongod(conn);

    conn =
        MongoRunner.runMongod({dbpath: dbpath, binVersion: "last-continuous", noCleanData: true});
    assert.neq(null,
               conn,
               "mongod was unable to start up with binary version=last-continuous" +
                   " and featureCompatibilityVersion=" + lastContinuousFCV);
    adminDB = conn.getDB("admin");
    checkFCV(adminDB, lastContinuousFCV);

    MongoRunner.stopMongod(conn);
}

function testReplSetDowngrade() {
    const replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();
    let primary = replTest.getPrimary();
    let primaryAdminDB = primary.getDB("admin");
    checkFCV(primaryAdminDB, latestFCV);

    jsTestLog("Downgrading FCV to last-continuous with {downgradeOnDiskChanges: true}");
    assert.commandWorked(primary.adminCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, downgradeOnDiskChanges: true}));
    checkFCV(primaryAdminDB, lastContinuousFCV);

    jsTestLog("Downgrading binary to last-continuous");
    replTest.upgradeSet({binVersion: "last-continuous"});
    primary = replTest.getPrimary();
    primaryAdminDB = primary.getDB("admin");
    checkFCV(primaryAdminDB, lastContinuousFCV);

    replTest.stopSet();
}

function testShardingDowngrade() {
    const st = new ShardingTest({
        shards: {rs0: {nodes: 2}},
        config: 1,
    });

    let configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    let shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    jsTestLog("Downgrading FCV to last-continuous with {downgradeOnDiskChanges: true}");
    assert.commandWorked(st.s.adminCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, downgradeOnDiskChanges: true}));
    checkFCV(configPrimaryAdminDB, lastContinuousFCV);
    checkFCV(shardPrimaryAdminDB, lastContinuousFCV);

    jsTestLog("Downgrading binary to last-continuous");
    st.upgradeCluster('last-continuous');
    configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, lastContinuousFCV);
    checkFCV(shardPrimaryAdminDB, lastContinuousFCV);

    st.stop();
}

testStandaloneDowngrade();
testReplSetDowngrade();
testShardingDowngrade();
}());
