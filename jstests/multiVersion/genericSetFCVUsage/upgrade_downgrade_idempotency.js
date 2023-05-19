/**
 * Tests the idempotency of the 'setFeatureCompatibilityVersion' command.
 * We execute the following steps for both upgrade and downgrade:
 * 1. Enable a failpoint to fail upgrading/downgrading.
 * 2. Issue a setFeatureCompatibilityVersion command, which upgrades/downgrades
 *    the replica set to a kUpgrading/kDowngrading intermediary state.
 * 3. The setFeatureCompatibilityVersion command fails without completing all
 *    upgrade/downgrade behavior.
 * 4. Disable the failpoint, and issue a succesful setFeatureCompatibilityVersion
 *    to finish upgrading/downgrading.
 */

(function() {
"use strict";

load("jstests/libs/write_concern_util.js");

function runTest(downgradeVersion) {
    const downgradeFCV = binVersionToFCV(downgradeVersion);
    const replTest = new ReplSetTest({name: jsTestName(), nodes: 2});
    replTest.startSet();
    replTest.initiate();

    let primary = replTest.getPrimary();
    // Enable failpoint to fail downgrading.
    let failpoint = configureFailPoint(primary, 'failDowngrading');
    assert.commandFailed(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Verify the node is in an intermediary state. If the response object has the 'targetVersion'
    // field, we are in a partially upgraded or downgraded state.
    checkFCV(primary.getDB("admin"), downgradeFCV, downgradeFCV);

    failpoint.off();

    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    // Verify the feature compatibility version transition is complete.
    checkFCV(primary.getDB("admin"), downgradeFCV);

    const latestFCV = binVersionToFCV('latest');
    // Enable failpoint to fail upgrading.
    failpoint = configureFailPoint(primary, 'failUpgrading');
    assert.commandFailed(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Verify the node is in an intermediary state. If the response object has the 'targetVersion'
    // field, we are in a partially upgraded or downgraded state.
    checkFCV(primary.getDB("admin"), downgradeFCV, latestFCV);

    failpoint.off();
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Verify the feature compatibility version transition is complete.
    checkFCV(primary.getDB("admin"), latestFCV);

    replTest.stopSet();
}

runTest('last-lts');
runTest('last-continuous');
}());
