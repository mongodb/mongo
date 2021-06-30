/*
 * Tests the following scenarios where the featureCompatibilityVersion document is rolled back and
 * verify that the in-memory and on-disk FCV stay consistent.
 * - the FCV document is rolled back from fully upgraded to upgrading
 * - the FCV document is rolled back from upgrading to fully downgraded
 * - the FCV document is rolled back from fully downgraded to downgrading
 * - the FCV document is rolled back from downgrading to fully upgraded
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/rollback_test.js");
    load("jstests/libs/check_log.js");
    load("jstests/libs/feature_compatibility_version.js");
    load('jstests/libs/parallel_shell_helpers.js');

    function setFCV(fcv) {
        // Let the server know to leave this connection alive on primary stepdowns.
        assert.commandWorked(db.adminCommand({ismaster: 1, hangUpOnStepDown: false}));
        assert.commandFailed(db.adminCommand({setFeatureCompatibilityVersion: fcv}));
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

        jsTestLog("Testing rolling back FCV from {version: " + lastStableFCV + ", targetVersion: " +
                  fromFCV + "} to {version: " + toFCV + "}");

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
        // The primary should have rolled back their FCV to be consistent with the rest of the
        // replica set.
        checkFCV(primaryAdminDB, toFCV);
        checkFCV(secondaryAdminDB, toFCV);

        let newPrimary = rollbackTest.getPrimary();
        // As a rule, we forbid downgrading a node while a node is still in the upgrading state and
        // vice versa. Ensure that the in-memory and on-disk FCV are consistent by checking that we
        // are able to set the FCV back to the original version.
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

        jsTestLog("Testing rolling back FCV from {version: " + fromFCV + "} to {version: " +
                  lastStableFCV + ", targetVersion: " + fromFCV + "}");

        // A failpoint to hang right before unsetting the targetVersion.
        assert.commandWorked(
            primary.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}));
        let setFCVInParallel = startParallelShell(funWithArgs(setFCV, fromFCV), primary.port);

        jsTestLog("Waiting to hang in failpoint - " + failPoint);
        checkLog.contains(primary,
                          "featureCompatibilityVersion - " + failPoint + " fail point enabled");
        // Clear ramlog so checkLog can't find log messages from the previous times this test was
        // run.
        assert.commandWorked(primary.adminCommand({clearLog: 'global'}));

        rollbackTest.transitionToRollbackOperations();
        // Turn off the failpoint so the primary will proceed to unset the targetVersion. This
        // update should never make it to the secondary.
        assert.commandWorked(primary.adminCommand({configureFailPoint: failPoint, mode: "off"}));

        assert.soon(function() {
            let featureCompatibilityVersion = getFCVFromDocument(primary);
            return !featureCompatibilityVersion.hasOwnProperty('targetVersion') &&
                featureCompatibilityVersion.version === fromFCV;
        }, "Failed waiting for server to unset the targetVersion or to set the FCV to " + fromFCV);
        rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
        // The secondary should never have received the update to unset the targetVersion.
        checkFCV(secondaryAdminDB, lastStableFCV, fromFCV);
        rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        setFCVInParallel();
        rollbackTest.transitionToSteadyStateOperations();
        // The primary should have rolled back their FCV to contain the targetVersion.
        checkFCV(primaryAdminDB, lastStableFCV, fromFCV);
        checkFCV(secondaryAdminDB, lastStableFCV, fromFCV);

        let newPrimary = rollbackTest.getPrimary();
        // As a rule, we forbid downgrading a node while a node is still in the upgrading state and
        // vice versa. Ensure that the in-memory and on-disk FCV are consistent by checking that
        // this rule is upheld after rollback.
        assert.commandFailedWithCode(
            newPrimary.adminCommand({setFeatureCompatibilityVersion: toFCV}),
            ErrorCodes.IllegalOperation);
    }

    const testName = jsTest.name();

    const rollbackTest = new RollbackTest(testName);

    // Tests the case where we roll back the FCV state from downgrading to fully upgraded.
    rollbackFCVFromDowngradingOrUpgrading(lastStableFCV, latestFCV);

    // Tests the case where we roll back the FCV state from upgrading to fully downgraded.
    rollbackFCVFromDowngradingOrUpgrading(latestFCV, lastStableFCV);

    // Tests the case where we roll back the FCV state from fully downgraded to downgrading.
    rollbackFCVFromDowngradedOrUpgraded(lastStableFCV, latestFCV, "hangWhileDowngrading");

    // Tests the case where we roll back the FCV state from fully upgraded to upgrading.
    rollbackFCVFromDowngradedOrUpgraded(latestFCV, lastStableFCV, "hangWhileUpgrading");

    rollbackTest.stop();
}());
