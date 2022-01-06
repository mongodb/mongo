/*
 * Tests the following scenarios where the featureCompatibilityVersion document is rolled back and
 * verify that the in-memory and on-disk FCV stay consistent.
 * - the FCV document is rolled back from fully upgraded to upgrading
 * - the FCV document is rolled back from upgrading to fully downgraded
 * - the FCV document is rolled back from fully downgraded to downgrading
 * - the FCV document is rolled back from downgrading to fully upgraded
 *
 *  @tags: [multiversion_incompatible]
 */

(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");

function setFCV(fcv) {
    assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: fcv}),
                                 ErrorCodes.InterruptedDueToReplStateChange);
}

// Using getParameter results in waiting for the current FCV to be majority committed.  In this
// test, it never will, so we need to get the FCV directly.
function getFCVFromDocument(conn) {
    return conn.getDB("admin").system.version.find().readConcern("local").toArray()[0];
}

// fromFCV refers to the FCV we will test rolling back from.
// toFCV refers to the FCV we will test rolling back to.
function rollbackFCVFromDowngradingOrUpgrading(fromFCV, toFCV) {
    let primary = rollbackTest.getPrimary();
    let secondary = rollbackTest.getSecondary();
    let primaryAdminDB = primary.getDB('admin');
    let secondaryAdminDB = secondary.getDB('admin');

    // Ensure the cluster starts at the correct FCV.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: toFCV}));
    // Wait until the config has propagated to the other nodes and the primary has learned of it, so
    // that the config replication check in 'setFeatureCompatibilityVersion' is satisfied. This is
    // only important since 'setFeatureCompatibilityVersion' is known to implicitly call internal
    // reconfigs as part of upgrade/downgrade behavior.
    rollbackTest.getTestFixture().waitForConfigReplication(primary);
    // Wait for the majority commit point to be updated on the secondary, because checkFCV calls
    // getParameter for the featureCompatibilityVersion, which will wait until the FCV change makes
    // it into the node's majority committed snapshot.
    rollbackTest.getTestFixture().awaitLastOpCommitted(undefined /* timeout */, [secondary]);

    jsTestLog("Testing rolling back FCV from {version: " + lastLTSFCV +
              ", targetVersion: " + fromFCV + "} to {version: " + toFCV + "}");

    rollbackTest.transitionToRollbackOperations();
    let setFCVInParallel = startParallelShell(funWithArgs(setFCV, fromFCV), primary.port);
    // Wait for the FCV update to be reflected on the primary. This should eventually be rolled
    // back.
    assert.soon(function() {
        let featureCompatibilityVersion = getFCVFromDocument(primary);
        return featureCompatibilityVersion.hasOwnProperty('targetVersion');
    }, "Failed waiting for the server to set the targetVersion: " + fromFCV);
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    // Secondaries should never have received the FCV update.
    checkFCV(secondaryAdminDB, toFCV);
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    setFCVInParallel();
    rollbackTest.transitionToSteadyStateOperations();
    // The primary should have rolled back their FCV to be consistent with the rest of the replica
    // set.
    checkFCV(primaryAdminDB, toFCV);
    checkFCV(secondaryAdminDB, toFCV);

    let newPrimary = rollbackTest.getPrimary();
    // As a rule, we forbid downgrading a node while a node is still in the upgrading state and
    // vice versa. Ensure that the in-memory and on-disk FCV are consistent by checking that we are
    // able to set the FCV back to the original version.
    assert.commandWorked(newPrimary.adminCommand({setFeatureCompatibilityVersion: toFCV}));
}

// fromFCV refers to the FCV we will test rolling back from.
// toFCV refers to the FCV we will test rolling back to.
function rollbackFCVFromDowngradedOrUpgraded(fromFCV, toFCV, failPoint) {
    let primary = rollbackTest.getPrimary();
    let secondary = rollbackTest.getSecondary();
    let primaryAdminDB = primary.getDB('admin');
    let secondaryAdminDB = secondary.getDB('admin');

    // Complete the upgrade/downgrade to ensure we are not in the upgrading/downgrading state.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: toFCV}));
    // Wait for the majority commit point to be updated on the secondary, because checkFCV calls
    // getParameter for the featureCompatibilityVersion, which will wait until the FCV change makes
    // it into the node's majority committed snapshot.
    rollbackTest.getTestFixture().awaitLastOpCommitted(undefined /* timeout */, [secondary]);

    jsTestLog("Testing rolling back FCV from {version: " + fromFCV +
              "} to {version: " + lastLTSFCV + ", targetVersion: " + fromFCV + "}");

    // A failpoint to hang right before unsetting the targetVersion.
    const hangBeforeUnsettingTargetVersion = configureFailPoint(primary, failPoint);
    let setFCVInParallel = startParallelShell(funWithArgs(setFCV, fromFCV), primary.port);
    hangBeforeUnsettingTargetVersion.wait();
    rollbackTest.transitionToRollbackOperations();
    // Turn off the failpoint so the primary will proceed to unset the targetVersion. This update
    // should never make it to the secondary.
    hangBeforeUnsettingTargetVersion.off();
    assert.soon(function() {
        let featureCompatibilityVersion = getFCVFromDocument(primary);
        return !featureCompatibilityVersion.hasOwnProperty('targetVersion') &&
            featureCompatibilityVersion.version === fromFCV;
    }, "Failed waiting for server to unset the targetVersion or to set the FCV to " + fromFCV);
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    // The secondary should never have received the update to unset the targetVersion.
    checkFCV(secondaryAdminDB, lastLTSFCV, fromFCV);
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    setFCVInParallel();
    rollbackTest.transitionToSteadyStateOperations();
    // The primary should have rolled back their FCV to contain the targetVersion.
    checkFCV(primaryAdminDB, lastLTSFCV, fromFCV);
    checkFCV(secondaryAdminDB, lastLTSFCV, fromFCV);

    let newPrimary = rollbackTest.getPrimary();
    // As a rule, we forbid downgrading a node while a node is still in the upgrading state and
    // vice versa. Ensure that the in-memory and on-disk FCV are consistent by checking that this
    // rule is upheld after rollback.
    assert.commandFailedWithCode(newPrimary.adminCommand({setFeatureCompatibilityVersion: toFCV}),
                                 5147403);
}

const testName = jsTest.name();

const rollbackTest = new RollbackTest(testName);

// Tests the case where we roll back the FCV state from downgrading to fully upgraded.
rollbackFCVFromDowngradingOrUpgrading(lastLTSFCV, latestFCV);

// Tests the case where we roll back the FCV state from upgrading to fully downgraded.
rollbackFCVFromDowngradingOrUpgrading(latestFCV, lastLTSFCV);

// Tests the case where we roll back the FCV state from fully downgraded to downgrading.
rollbackFCVFromDowngradedOrUpgraded(lastLTSFCV, latestFCV, "hangWhileDowngrading");

// Tests the case where we roll back the FCV state from fully upgraded to upgrading.
rollbackFCVFromDowngradedOrUpgraded(latestFCV, lastLTSFCV, "hangWhileUpgrading");

rollbackTest.stop();
}());
