/**
 * Test the automatic dry-run behavior during setFeatureCompatibilityVersion command,
 * along with the skipDryRun parameter and failpoints for dry-run failures.
 *
 * @tags: [featureFlagSetFcvDryRunMode]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const failCases = [
    {
        fromFCV: latestFCV,
        toFCV: lastLTSFCV,
        failPointName: "failDowngradeValidationDueToIncompatibleFeature",
        expectedError: ErrorCodes.CannotDowngrade,
    },
    {
        fromFCV: lastLTSFCV,
        toFCV: latestFCV,
        failPointName: "failUpgradeValidationDueToIncompatibleFeature",
        expectedError: ErrorCodes.CannotUpgrade,
    },
];

/**
 * Tests the behavior when automatic dry-run fails during FCV transitions.
 * Ensures the FCV remains unchanged.
 */
function testDryRunFailStopsTransition(
    conn, shardConn, fromFCV, toFCV, failPointName, expectedError) {
    const db = conn.getDB("admin");

    jsTestLog(`Setting initial FCV to ${fromFCV}.`);
    assert.commandWorked(db.runCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}));

    let failpoint = configureFailPoint(shardConn, failPointName);

    jsTestLog(`Testing automatic dry-run during FCV transition: ${fromFCV} → ${
        toFCV}, expecting failure`);
    const result = db.runCommand({setFeatureCompatibilityVersion: toFCV, confirm: true});
    assert.commandFailedWithCode(result, expectedError);
    checkFCV(db, fromFCV);  // FCV should remain unchanged

    failpoint.off();
    jsTestLog("Automatic dry-run failure test completed.");

    // Reset FCV to latestFCV for consistency across tests
    assert.commandWorked(db.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}

/**
 * Tests conflicting parameters `dryRun` and `skipDryRun` in setFCV command. They cannot be true at
 * the same time because skipDryRun is used to avoid the default dryRun in setFCV command
 */
function testConflictingParametersWithSkipDryRun(conn) {
    const db = conn.getDB("admin");

    jsTestLog("Testing conflicting `dryRun` and `skipDryRun` parameters.");
    const result = db.runCommand({
        setFeatureCompatibilityVersion: lastLTSFCV,
        dryRun: true,
        skipDryRun: true,
        confirm: true
    });
    assert.commandFailedWithCode(result, ErrorCodes.InvalidOptions);

    checkFCV(db, latestFCV);  // FCV should remain unchanged when the command fails.

    jsTestLog("Conflicting parameter tests completed.");
}

// Tests a valid downgrade transition using `skipDryRun`, which skips the automatic validation
// step and completes successfully
function testSuccessWithSkipDryRun(conn) {
    const db = conn.getDB("admin");

    jsTestLog(
        "Testing successful FCV downgrade using `skipDryRun` parameter: latestFCV → lastLTSFCV.");
    const skipDryRunSuccessResult = db.runCommand({
        setFeatureCompatibilityVersion: lastLTSFCV,
        skipDryRun: true,
        confirm: true,
    });

    assert.commandWorked(skipDryRunSuccessResult);
    checkFCV(db, lastLTSFCV);  // Ensure the FCV transitioned successfully.

    // Reset FCV to latestFCV for consistency across tests
    assert.commandWorked(db.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}
/**
 * Tests a downgrade attempt with `skipDryRun` and a failpoint enabled, which simulates an invalid
 * transition and results in a failure, leaving the FCV in a transitional state due to the skipped
 * validation.
 */
function testFailureWithSkipDryRun(conn, shardConn, failPointName, expectedError) {
    const db = conn.getDB("admin");

    let failpoint = configureFailPoint(shardConn, failPointName);

    jsTestLog(
        "Testing unsuccessful FCV downgrade using `skipDryRun` parameter: latestFCV → lastLTSFCV.");
    const skipDryRunFailResult = db.runCommand({
        setFeatureCompatibilityVersion: lastLTSFCV,
        skipDryRun: true,
        confirm: true,
    });
    assert.commandFailedWithCode(skipDryRunFailResult, expectedError);

    checkFCV(db, lastLTSFCV, lastLTSFCV);  // if dryRun is skipped and an error is raised, the FCV
                                           // remains in a transitional downgrading state.

    // If the user runs an explicit dryRun at this point, an error will be thrown and their FCV
    // stays in the transitional state
    assert.commandFailedWithCode(db.runCommand({
        setFeatureCompatibilityVersion: lastLTSFCV,
        dryRun: true,
        confirm: true,
    }),
                                 expectedError);
    checkFCV(db, lastLTSFCV, lastLTSFCV);

    failpoint.off();

    // If the user runs an explicit dryRun at this point, it will succeed but still keep the FCV in
    // the transitional state
    assert.commandWorked(
        db.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, dryRun: true, confirm: true}));
    checkFCV(db, lastLTSFCV, lastLTSFCV);

    jsTestLog("SkipDryRun tests completed successfully.");

    // Reset FCV to latestFCV for consistency across tests
    assert.commandWorked(db.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}

// shardConn is used for specifying the shard in which enabling the failpoints
function runDryRunTests(conn, shardConn) {
    jsTestLog(">>> Testing skipDryRun parameter");
    testSuccessWithSkipDryRun(conn);
    testFailureWithSkipDryRun(conn,
                              shardConn,
                              "failDowngradeValidationDueToIncompatibleFeature",
                              ErrorCodes.CannotDowngrade);

    for (const {fromFCV, toFCV, failPointName, expectedError} of failCases) {
        jsTestLog(">>> Testing expected dryRun failure scenario");
        testDryRunFailStopsTransition(
            conn, shardConn, fromFCV, toFCV, failPointName, expectedError);
    }

    jsTestLog(">>> Testing conflicting parameters dryRun and skipDryRun");
    testConflictingParametersWithSkipDryRun(conn);
}

function testAllTopologies() {
    jsTestLog("Starting dry-run test for all topologies.");

    // Standalone topology tests
    {
        jsTestLog("Testing standalone topology");
        const conn = MongoRunner.runMongod({});
        runDryRunTests(conn, conn);
        MongoRunner.stopMongod(conn);
        jsTestLog("Dry-run tests for standalone topology completed successfully.");
    }

    // Sharded cluster topology tests
    {
        jsTestLog("Testing sharded cluster topology");
        const st = new ShardingTest({shards: 2});
        runDryRunTests(st.s, st.rs0.getPrimary());
        runDryRunTests(st.s, st.configRS.getPrimary());
        st.stop();
        jsTestLog("Dry-run tests for sharded cluster topology completed successfully.");
    }

    // Replica set topology tests
    {
        jsTestLog("Testing replica set topology");
        const rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        runDryRunTests(rst.getPrimary(), rst.getPrimary());
        rst.stopSet();
        jsTestLog("Dry-run tests for standalone topology completed successfully.");
    }

    jsTestLog("Dry-run test execution completed.");
}
testAllTopologies();
