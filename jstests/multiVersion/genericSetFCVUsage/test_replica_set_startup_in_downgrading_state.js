/*
 * Tests startup with a node in downgrading state.
 * Starts a replica set with 2 nodes.
 *
 * @tags: [requires_fcv_70]
 */
import "jstests/multiVersion/libs/verify_versions.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";

function runReplicaSet() {
    let fcvDoc;

    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: "latest"}});

    rst.startSet();
    rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    const primaryAdminDB = rst.getPrimary().getDB("admin");
    const primary = rst.getPrimary();
    const secondaryAdminDB = rst.getSecondary().getDB("admin");

    fcvDoc = primaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Primary's version before downgrading: ${tojson(fcvDoc)}`);
    checkFCV(primaryAdminDB, latestFCV);

    // Set the failDowngrading failpoint so that the downgrading will fail.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));

    // Start downgrading. It will fail.
    jsTestLog("setFCV command called. Downgrading from latestFCV to lastLTSFCV.");
    assert.commandFailed(
        primaryAdminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    fcvDoc = primaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Primary's version after downgrading: ${tojson(fcvDoc)}`);
    checkFCV(primaryAdminDB, lastLTSFCV, lastLTSFCV);

    rst.awaitReplication();

    fcvDoc = secondaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Secondary's version after downgrading: ${tojson(fcvDoc)}`);
    checkFCV(secondaryAdminDB, lastLTSFCV, lastLTSFCV);

    jsTestLog("Stopping the primary.");
    const primaryId = rst.getNodeId(primary);
    rst.stop(primaryId, {forRestart: true});
    rst.waitForState(primary, ReplSetTest.State.DOWN);

    jsTestLog("Restarting the primary.");
    rst.restart(primaryId, {}, true /* wait */);
    rst.awaitSecondaryNodes(null, [primary]);

    fcvDoc = primaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Old primary's version after restarting: ${tojson(fcvDoc)}`);
    checkFCV(primaryAdminDB, lastLTSFCV, lastLTSFCV);

    fcvDoc = secondaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`New primary's version after restarting: ${tojson(fcvDoc)}`);
    checkFCV(secondaryAdminDB, lastLTSFCV, lastLTSFCV);

    // Upgrade the replica set to upgraded (latestFCV).
    const newPrimaryAdminDB = rst.getPrimary().getDB("admin");
    jsTestLog("setFCV command called. Finish upgrading to latestFCV.");
    assert.commandWorked(
        newPrimaryAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    rst.awaitReplication();

    fcvDoc = primaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`Old primary's version after successfully upgrading: ${tojson(fcvDoc)}`);
    checkFCV(primaryAdminDB, latestFCV);

    fcvDoc = secondaryAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(`New primary's version after successfully upgrading: ${tojson(fcvDoc)}`);
    checkFCV(secondaryAdminDB, latestFCV);

    rst.stopSet();
}

runReplicaSet();
