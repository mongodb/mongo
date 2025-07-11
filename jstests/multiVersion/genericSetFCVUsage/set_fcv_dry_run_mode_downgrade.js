/**
 * Tests the `dryRun` option of the setFeatureCompatibilityVersion command.
 * Tests downgrading case.
 *
 * @tags: [featureFlagSetFcvDryRunMode]
 */

function runTest() {
    jsTestLog("Testing dryRun mode for setFeatureCompatibilityVersion command.");

    // Start a standalone MongoDB server
    const conn = MongoRunner.runMongod({});
    const adminDB = conn.getDB("admin");

    const initialFCV = latestFCV;

    jsTestLog(`Setting initial FCV to ${initialFCV}.`);
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: initialFCV, confirm: true}));

    jsTestLog("CASE 1: Validate Dry-run success for downgrade from latestFCV to lastLTSFCV.");
    let res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastLTSFCV, dryRun: true, confirm: true});
    assert.commandWorked(res, "Dry-run did not succeed.");

    checkFCV(adminDB, initialFCV);  // FCV should remain unchanged post-dry-run

    jsTestLog(
        "CASE 2: Validate Dry-run success for downgrade from latestFCV to lastContinuousFCV.");
    res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, dryRun: true, confirm: true});
    assert.commandWorked(res, "Dry-run did not succeed.");

    checkFCV(adminDB, initialFCV);  // FCV should remain unchanged post-dry-run

    jsTestLog("CASE 3: Validate dry-run failure when fail point is enabled.");

    jsTestLog("Enabling fail point to simulate dry-run validation failure.");
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failDowngradeValidationDueToIncompatibleFeature",
        mode: "alwaysOn",
    }));

    res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastLTSFCV, dryRun: true, confirm: true});

    jsTestLog("Validating the simulated failure response.");
    assert.commandFailedWithCode(
        res, ErrorCodes.CannotDowngrade, "Expected dry-run validation to fail.");

    checkFCV(adminDB, initialFCV);  // FCV should remain unchanged post-dry-run

    jsTestLog("Disabling fail point and cleaning up.");
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failDowngradeValidationDueToIncompatibleFeature",
        mode: "off",
    }));

    jsTestLog("Stopping the MongoDB server.");
    MongoRunner.stopMongod(conn);
}

runTest();
