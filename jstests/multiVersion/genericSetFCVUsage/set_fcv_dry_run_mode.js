/**
 * Test for both downgrading and upgrading FCV cases using the `dryRun` option of
 * the setFeatureCompatibilityVersion command.
 *
 * @tags: [featureFlagSetFcvDryRunMode]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const successfulCases = [
    {fromFCV: latestFCV, toFCV: lastLTSFCV}, // Downgrade from latest to lastLTS
    {fromFCV: latestFCV, toFCV: lastContinuousFCV}, // Downgrade from latest to lastContinuous
    {fromFCV: lastLTSFCV, toFCV: latestFCV}, // Upgrade from lastLTS to latest
    {
        fromFCV: lastLTSFCV,
        toFCV: lastContinuousFCV,
        fromConfigServer: true,
    }, // Upgrade from lastLTS to lastContinuous . fromConfigServer is needed because this
    // transition is only allowed as part of internal operations (`addShard`).

    {fromFCV: lastContinuousFCV, toFCV: latestFCV}, // Upgrade from lastContinuous to latest
    {fromFCV: latestFCV, toFCV: latestFCV}, // Same requested and actual version
];

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

if (lastLTSFCV !== lastContinuousFCV) {
    failCases.push({
        fromFCV: lastLTSFCV,
        toFCV: lastContinuousFCV,
        expectedError: 5147403,
    });
}

function testSuccessfulDryRun(conn, fromFCV, toFCV, fromConfigServer) {
    const db = conn.getDB("admin");

    jsTestLog(`Setting initial FCV to ${fromFCV}.`);
    assert.commandWorked(db.runCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}));

    jsTestLog(`Performing dry-run validation for transition from ${fromFCV} to ${toFCV}.`);

    const commandObj = {setFeatureCompatibilityVersion: toFCV, dryRun: true};

    // Include `fromConfigServer` if specified in the test case
    if (fromConfigServer) {
        commandObj.fromConfigServer = true;
    }

    const res = db.runCommand(commandObj);
    assert.commandWorked(res, "Dry-run validation did not succeed.");
    checkFCV(db, fromFCV); // FCV should remain unchanged post-dry-run

    jsTestLog(`Dry-run completed successfully.`);

    // Reset FCV to latestFCV for consistency across tests
    assert.commandWorked(db.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}

function testFailedDryRun(conn, fromFCV, toFCV, failPointName, expectedError, shardConn) {
    const db = conn.getDB("admin");
    jsTestLog(`Setting initial FCV to ${fromFCV}.`);
    assert.commandWorked(db.runCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}));

    if (failPointName) {
        jsTestLog(`Enabling failpoint '${failPointName}' to simulate validation failure.`);
        assert.commandWorked(
            shardConn.adminCommand({
                configureFailPoint: failPointName,
                mode: "alwaysOn",
            }),
        );
    }
    jsTestLog(`Performing dry-run validation for transition from ${fromFCV} to ${toFCV} expecting failure.`);
    const res = db.runCommand({
        setFeatureCompatibilityVersion: toFCV,
        dryRun: true,
    });

    jsTestLog("Validating the error code returned from the dry-run.");
    assert.commandFailedWithCode(res, expectedError, `Expected error during dry-run validation.`);

    checkFCV(db, fromFCV); // FCV should remain unchanged post-dry-run

    if (failPointName) {
        jsTestLog(`Disabling failpoint '${failPointName}'.`);
        assert.commandWorked(
            shardConn.adminCommand({
                configureFailPoint: failPointName,
                mode: "off",
            }),
        );
    }

    jsTestLog(`Failure simulation for dry-run validation completed successfully.`);

    // Reset FCV to latestFCV for consistency across tests
    assert.commandWorked(db.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}

// shardConn is used for specifying the shard in which enabling the failpoints
function runDryRunTests(conn, shardConn) {
    for (const {fromFCV, toFCV, fromConfigServer} of successfulCases) {
        jsTestLog(`>>> Testing dry-run transition: ${fromFCV} → ${toFCV}`);
        testSuccessfulDryRun(conn, fromFCV, toFCV, fromConfigServer);
    }

    for (const {fromFCV, toFCV, failPointName, expectedError} of failCases) {
        jsTestLog(`>>> Testing expected failure scenario: ${fromFCV} → ${toFCV}`);
        testFailedDryRun(conn, fromFCV, toFCV, failPointName, expectedError, shardConn);
    }
}

function testAllTopologies() {
    jsTestLog("Starting dry-run test for all topologies.");

    // Standalone topology tests
    {
        jsTestLog("Testing standalone topology");
        const conn = MongoRunner.runMongod({});
        runDryRunTests(conn, conn);
        MongoRunner.stopMongod(conn);
        jsTestLog(`Dry-run tests for standalone topology completed successfully.`);
    }

    // Sharded cluster topology tests
    {
        jsTestLog("Testing sharded cluster topology");
        const st = new ShardingTest({shards: 2});
        runDryRunTests(st.s, st.rs0.getPrimary());
        runDryRunTests(st.s, st.configRS.getPrimary());
        st.stop();
        jsTestLog(`Dry-run tests for sharded cluster topology completed successfully.`);
    }

    // Replica set topology tests
    {
        jsTestLog("Testing replica set topology");
        const rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        runDryRunTests(rst.getPrimary(), rst.getPrimary());
        rst.stopSet();
        jsTestLog(`Dry-run tests for standalone topology completed successfully.`);
    }

    jsTestLog("Dry-run test execution completed.");
}

testAllTopologies();
