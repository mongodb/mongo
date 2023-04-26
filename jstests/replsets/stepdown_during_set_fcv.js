/**
 * Tests that a stepdown during the execution of the setFeatureCompatibilityVersion command causes
 * it to fail gracefully.
 *
 * @tags: [
 *    multiversion_incompatible
 * ]
 *
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

function runTest(downgradeFCV) {
    const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();

    const failpoint = configureFailPoint(primary, 'hangBeforeUpdatingFcvDoc');

    jsTestLog("Issue a setFeatureCompatibilityVersion command that will wait on the " +
              "hangBeforeUpdatingFcvDoc failpoint");

    const parallelFn = `
    assert.commandFailedWithCode(
        db.adminCommand({setFeatureCompatibilityVersion: "${downgradeFCV}"}),
        ErrorCodes.InterruptedDueToReplStateChange); `;

    const awaitShell = startParallelShell(parallelFn, primary.port);

    failpoint.wait();

    jsTestLog("Stepping down the primary");
    assert.commandWorked(primary.adminCommand({replSetStepDown: 10 * 60, force: 1}));
    rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    rst.awaitNodesAgreeOnPrimary();

    primary = rst.getPrimary();
    const secondary = rst.getSecondaries()[0];

    jsTestLog("Issue a setFeatureCompatibilityVersion command on the new primary");
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    rst.awaitReplication();

    jsTestLog("Unset the failpoint on the former primary so it finishes running " +
              "setFeatureCompatibilityVersion");
    failpoint.off();
    awaitShell();

    jsTestLog("Verify that both nodes are running with downgraded featureCompatibilityVersion");
    rst.awaitReplication();
    checkFCV(primary.getDB("admin"), downgradeFCV);
    checkFCV(secondary.getDB("admin"), downgradeFCV);

    rst.stopSet();
}

jsTestLog("Running test against lastLTSFCV");
runTest(lastLTSFCV);
if (lastLTSFCV !== lastContinuousFCV) {
    jsTestLog("Running test against lastContinuousFCV");
    runTest(lastContinuousFCV);
}
})();
