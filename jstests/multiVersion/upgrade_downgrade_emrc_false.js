/**
 * Tests upgrade/downgrade for valid eMRC=false usage.
 *
 * Verifies the following cases:
 * 1. A user should be able to run a last-lts node with eMRC=F, shut down the node, and restart
 *    it with the latest binary and emrc=T.
 * 2. A user should be able to run a latest node with eMRC=T, downgrade the FCV to last-lts, shut it
 *    down and restart it as a last-lts node with eMRC=F.
 *
 * TODO SERVER-53748: Remove this test once 5.0 becomes last-lts.
 */
(function() {
"use strict";
load('jstests/multiVersion/libs/verify_versions.js');  // For 'assert.binVersion()'

const dbName = "test";
const collName = jsTestName();

function doMajorityRead(emrc, priDB) {
    if (emrc) {
        assert.commandWorked(priDB.runCommand({find: collName, readConcern: {level: "majority"}}));
    } else {
        assert.commandFailedWithCode(
            priDB.runCommand({find: collName, readConcern: {level: "majority"}}),
            ErrorCodes.ReadConcernMajorityNotEnabled);
    }
}

function runReplicaSetTest(upgrading) {
    const startVersion = upgrading ? "last-lts" : "latest";
    const endVersion = upgrading ? "latest" : "last-lts";
    let emrc = upgrading ? false : true;

    jsTestLog("Starting a replica set with " + startVersion + " binaries and eMRC=" + emrc);
    const rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {binVersion: startVersion, enableMajorityReadConcern: emrc}});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const adminDB = primary.getDB("admin");
    const primaryColl = primaryDB[collName];

    // Insert document using writeConcern majority.
    assert.commandWorked(primaryColl.insert({a: 1}, {writeConcern: {w: "majority"}}));

    // If eMRC=true, verify that we can do a majority read.
    doMajorityRead(emrc, primaryDB);

    // Set FCV to last-lts if downgrading from latest to last-lts.
    if (!upgrading) {
        const startFCV = binVersionToFCV(startVersion);
        const endFCV = binVersionToFCV(endVersion);

        checkFCV(adminDB, startFCV);

        jsTestLog("Downgrading FCV to " + endFCV);
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: endFCV}));

        checkFCV(adminDB, endFCV);
    }

    emrc = !emrc;
    jsTestLog("Restarting replica set with " + endVersion + " binaries and eMRC=" + emrc);
    rst.stopSet();
    rst.startSet({binVersion: endVersion, enableMajorityReadConcern: emrc});
    rst.initiate();

    // Verify that all nodes are in the endVersion.
    for (const node of rst.nodes) {
        assert.binVersion(node, endVersion);
    }

    const newPrimary = rst.getPrimary();
    const newPrimaryDB = newPrimary.getDB(dbName);
    const newAdminDB = newPrimary.getDB("admin");
    const newPrimaryColl = newPrimaryDB[collName];
    // Always check that the restarted set is using last-lts FCV.
    checkFCV(newAdminDB, binVersionToFCV("last-lts"));

    // Insert document using writeConcern majority.
    assert.commandWorked(newPrimaryColl.insert({b: 2}, {writeConcern: {w: "majority"}}));

    // If eMRC=true, verify that we can do a majority read.
    doMajorityRead(emrc, newPrimaryDB);

    rst.stopSet();
}

// Test last-lts eMRC=false upgrade to latest eMRC=true.
runReplicaSetTest(true /* upgrading */);
// Test latest eMRC=true downgrade to last-lts eMRC=false.
runReplicaSetTest(false /* upgrading */);
})();