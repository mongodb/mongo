/**
 * Tests the `dryRun` option of the setFeatureCompatibilityVersion command.
 * Tests upgrading case.
 *
 * @tags: [featureFlagSetFcvDryRunMode]
 */

function runTest() {
    jsTestLog("Testing dryRun mode for setFeatureCompatibilityVersion command.");

    // Start a standalone MongoDB server
    const conn = MongoRunner.runMongod({});
    const adminDB = conn.getDB("admin");

    jsTestLog(`Setting initial FCV to ${lastLTSFCV}.`);
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    jsTestLog("CASE 1: Validate Dry-run success for upgrade from lastLTSFCV to latestFCV.");
    let res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: latestFCV, dryRun: true, confirm: true});
    assert.commandWorked(res, "Dry-run did not succeed.");

    checkFCV(adminDB, lastLTSFCV);  // FCV should remain unchanged post-dry-run

    jsTestLog(
        "CASE 2A: Validate Dry-run success for upgrade from lastLTSFCV to lastContinuousFCV (from config server).");
    res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, dryRun: true, fromConfigServer: true});
    assert.commandWorked(res, "Dry-run did not succeed.");

    checkFCV(adminDB, lastLTSFCV);  // FCV should remain unchanged post-dry-run

    jsTestLog(
        "CASE 2B: Validate Dry-run failure for upgrade from lastLTSFCV to lastContinuousFCV (not from config server).");
    res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, dryRun: true, confirm: true});

    assert.commandFailedWithCode(res, 5147403, "Expected dry-run validation to fail.");

    checkFCV(adminDB, lastLTSFCV);  // FCV should remain unchanged post-dry-run

    jsTestLog(`Setting FCV to ${latestFCV}.`);
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    jsTestLog(`Setting FCV to ${lastContinuousFCV}.`);
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastContinuousFCV, confirm: true}));

    jsTestLog("CASE 3: Validate Dry-run success for upgrade from lastContinuousFCV to latestFCV.");
    res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: latestFCV, dryRun: true, confirm: true});
    assert.commandWorked(res, "Dry-run did not succeed.");

    checkFCV(adminDB, lastContinuousFCV);  // FCV should remain unchanged post-dry-run

    jsTestLog("CASE 4: Validate dry-run failure when fail point is enabled.");

    jsTestLog("Enabling fail point to simulate dry-run validation failure.");
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failUpgradeValidationDueToIncompatibleFeature",
        mode: "alwaysOn",
    }));

    res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: latestFCV, dryRun: true, confirm: true});

    jsTestLog("Validating the simulated failure response.");
    assert.commandFailedWithCode(
        res, ErrorCodes.CannotUpgrade, "Expected dry-run validation to fail.");

    checkFCV(adminDB, lastContinuousFCV);  // FCV should remain unchanged post-dry-run

    jsTestLog("Disabling fail point and cleaning up.");
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failUpgradeValidationDueToIncompatibleFeature",
        mode: "off",
    }));

    jsTestLog(
        "CASE 5: Validate Dry-run success when actual version and requested version are the same.");
    res = adminDB.runCommand(
        {setFeatureCompatibilityVersion: lastContinuousFCV, dryRun: true, confirm: true});

    assert.commandWorked(res, "Dry-run did not succeed.");
    jsTestLog("Stopping the MongoDB server.");
    MongoRunner.stopMongod(conn);
}

runTest();
