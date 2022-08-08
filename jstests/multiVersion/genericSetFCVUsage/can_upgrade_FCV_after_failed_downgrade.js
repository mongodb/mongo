/**
 * Tests for the new fcv change path added:
 * kDowngradingFromLatestToLastLTS -> kUpgradingFromLastLTSToLatest -> kLatest
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const latest = "latest";

function downgradingToUpgradingTest(conn, adminDB) {
    // 1) startup: latest
    let fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("1) current FCV (should be latest):");
    printjson(fcvDoc);
    checkFCV(adminDB, latestFCV);

    // 2) should be stuck in downgrading from latest to lastLTS
    assert.commandWorked(  // failpoint: fail after transitional state
        conn.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandFailed(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    fcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("2) current FCV (should be downgrading):");
    printjson(fcvDoc);
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV);

    // 3) fcv should be set to latest
    assert.commandWorked(conn.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    let newFcvDoc = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog("3) current FCV (should be upgraded to latest):");
    printjson(fcvDoc);
    checkFCV(adminDB, latestFCV);

    // if config server, also check that timestamp from fcvDoc should be "earlier" (return -1)
    if (fcvDoc.changeTimestamp != null) {
        assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);
    }
}

function runStandaloneTest() {
    const conn = MongoRunner.runMongod({binVersion: latest});
    assert.neq(
        null, conn, "mongod was unable to start up with version=" + latest + " and no data files");
    const adminDB = conn.getDB("admin");

    downgradingToUpgradingTest(conn, adminDB);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    const rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    const primaryAdminDB = rst.getPrimary().getDB("admin");
    const primary = rst.getPrimary();

    downgradingToUpgradingTest(primary, primaryAdminDB);

    rst.stopSet();
}

function runShardingTestTimestamp() {
    const st =
        new ShardingTest({shards: {rs0: {nodes: [{binVersion: latest}, {binVersion: latest}]}}});
    const mongosAdminDB = st.s.getDB("admin");
    const configPrimary = st.configRS.getPrimary();
    const shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    checkFCV(shardPrimaryAdminDB, latestFCV);

    downgradingToUpgradingTest(configPrimary, mongosAdminDB);

    /* check that a new timestamp is always generated with each setFCV call */
    let fcvDoc;
    let newFcvDoc;
    // 1) calling downgrade twice (one with failpoint)
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    fcvDoc = mongosAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failDowngrading', mode: "off"}));
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    newFcvDoc = mongosAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    checkFCV(mongosAdminDB, lastLTSFCV);
    // timestamp from fcvDoc should be "earlier" (return -1)
    assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);

    // 2) calling upgrade twice (one with failpoint)
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failUpgrading', mode: "alwaysOn"}));
    assert.commandFailed(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    fcvDoc = mongosAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: 'failUpgrading', mode: "off"}));
    assert.commandWorked(mongosAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
    newFcvDoc = mongosAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    checkFCV(mongosAdminDB, latestFCV);
    // timestamp from fcvDoc should be "earlier" (return -1)
    assert(timestampCmp(fcvDoc.changeTimestamp, newFcvDoc.changeTimestamp) == -1);

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTestTimestamp();
})();
