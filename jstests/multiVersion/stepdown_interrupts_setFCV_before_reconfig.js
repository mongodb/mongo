/**
 * Tests a scenario where stepdown interrupts the setFCV command after transitioning to the
 * intermediary kUpgrading/kDowngrading state but before the reconfig to change the delay field
 * name. In this state, a subsequent stepup attempt may have an FCV incompatible field name.
 * Since we are expected to run the setFCV command to finish the upgrade/downgrade procedure,
 * the delay field name will eventually be changed to the correct value, so it's safe to
 * temporarily skip the FCV compatibility check for stepup reconfigs instead of crashing the
 * server.
 *
 */

(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

function runSetFCV(targetVersion) {
    assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: targetVersion}),
                                 ErrorCodes.InterruptedDueToReplStateChange);
}

function checkForSecondaryDelaySecs(primary) {
    let config = primary.adminCommand({replSetGetConfig: 1}).config;
    assert.eq(config.members[0].secondaryDelaySecs, 0, config);
    assert(!config.members[0].hasOwnProperty('slaveDelay'), config);
}

function checkForSlaveDelay(primary) {
    let config = primary.adminCommand({replSetGetConfig: 1}).config;
    assert.eq(config.members[0].slaveDelay, 0, config);
    assert(!config.members[0].hasOwnProperty('secondaryDelaySecs'), config);
}

const replTest = new ReplSetTest({name: jsTestName(), nodes: 1});
replTest.startSet();
replTest.initiateWithHighElectionTimeout();
let primary = replTest.getPrimary();

jsTestLog("Testing stepdown interrupting FCV downgrade.");
// Enable failpoint to hang after transitioning to kDowngrading.
let failpoint = configureFailPoint(primary, 'hangAfterStartingFCVTransition');
let setFCVThread = startParallelShell(funWithArgs(runSetFCV, lastLTSFCV), primary.port);
failpoint.wait();

// Verify the node is in a partially downgraded state by checking that the 'targetVersion'
// was set to 'lastLTSFCV'.
checkFCV(primary.getDB("admin"), lastLTSFCV /* version */, lastLTSFCV /* targetVersion */);

// Interrupt the setFCV command with a stepdown.
assert.commandWorked(primary.adminCommand({replSetStepDown: 1, force: true}));
failpoint.off();
setFCVThread();
replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

// The automatic reconfig on stepup should be able to succeed, resulting in the
// 'secondaryDelaySecs' field still being present in the config even though we're in the
// kDowngrading FCV.
replTest.stepUp(primary);
primary = replTest.getPrimary();
checkForSecondaryDelaySecs(primary);

jsTestLog("Complete FCV downgrade.");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Verify the feature compatibility version transition is complete. Once we've completed the
// downgrade, the delay field should have been properly renamed to 'slaveDelay'.
checkFCV(primary.getDB("admin"), lastLTSFCV);
checkForSlaveDelay(primary);

jsTestLog("Testing stepdown interrupting FCV upgrade.");
// Enable failpoint to hang after transitioning to kUpgrading.
failpoint = configureFailPoint(primary, 'hangAfterStartingFCVTransition');
setFCVThread = startParallelShell(funWithArgs(runSetFCV, latestFCV), primary.port);
failpoint.wait();

// Verify the node is in a partially upgraded state by checking that the 'targetVersion'
// was set to 'latestFCV'.
checkFCV(primary.getDB("admin"), lastLTSFCV /* version */, latestFCV /* targetVersion */);

// Interrupt the setFCV command with a stepdown.
assert.commandWorked(primary.adminCommand({replSetStepDown: 1, force: true}));
failpoint.off();
setFCVThread();
replTest.waitForState(primary, ReplSetTest.State.SECONDARY);

// The automatic reconfig on stepup should be able to succeed, resulting in the 'slaveDelay'
// field still being present in the config even though we're in the kUpgrading FCV.
replTest.stepUp(primary);
primary = replTest.getPrimary();
checkForSlaveDelay(primary);

jsTestLog("Complete FCV upgrade.");
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Verify the feature compatibility version transition is complete. Once we've completed the
// upgrade, the delay field should have been properly renamed to 'secondaryDelaySecs'.
checkFCV(primary.getDB("admin"), latestFCV);
checkForSecondaryDelaySecs(primary);

replTest.stopSet();
}());
