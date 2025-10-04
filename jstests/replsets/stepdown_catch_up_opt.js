/*
 * Test to ensure the secondaryCatchUpPeriodSecs option in the replSetStepDown command is parsed
 * correctly, and will affect the delay appropriately.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

let name = "stepdown_catch_up_opt";
// Only 2 nodes, so that we can control whether the secondary is caught up.
let replTest = new ReplSetTest({name: name, nodes: 2});
replTest.startSet();
replTest.initiate();
replTest.awaitSecondaryNodes();
let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// Error codes we expect to see.

// If the secondary is not caught up.
const noCaughtUpSecondariesCode = ErrorCodes.ExceededTimeLimit;

// If the stepdown period is shorter than the secondaryCatchUpPeriodSecs argument.
let stepDownPeriodTooShortCode = 2;

// If we give a string as an argument instead of an integer.
let stringNotIntCode = 14;

// Expect a failure with a string argument.
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: "STR"}),
    stringNotIntCode,
    "Expected string argument to secondaryCatchupPeriodSecs to fail.",
);

// Expect a failure with a longer secondaryCatchupPeriodSecs than the stepdown period.
assert.commandFailedWithCode(
    primary.getDB("admin").runCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: 20}),
    stepDownPeriodTooShortCode,
    "Expected replSetStepDown to fail given a stepdown time shorter than" + " secondaryCatchUpPeriodSecs",
);

jsTestLog("Stop secondary syncing.");
stopServerReplication(secondary);

function disableFailPoint() {
    restartServerReplication(secondary);
}

// If any of these assertions fail, we need to disable the fail point in order for the mongod to
// shut down.
try {
    jsTestLog("Write to primary to make secondary out of sync.");
    assert.commandWorked(primary.getDB("test").foo.insert({i: 1}), "Failed to insert document.");
    sleep(1000);
    // Secondary is now at least 1 second behind.

    jsTestLog("Try to step down.");
    let startTime = new Date();
    assert.commandFailedWithCode(
        primary.getDB("admin").runCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: 2}),
        noCaughtUpSecondariesCode,
        "Expected replSetStepDown to fail, since no secondaries should be caught up.",
    );
    let endTime = new Date();

    // Ensure it took at least 2 second to time out. Adjust the timeout a little bit
    // for the precision issue of clock on Windows 2K8.
    assert.lte(1.95, (endTime - startTime) / 1000, "Expected replSetStepDown command to fail after 2 seconds.");
} catch (err) {
    disableFailPoint();
    throw err;
}

disableFailPoint();

// Make sure the primary hasn't changed, since all stepdowns should have failed.
let primaryStatus = primary.getDB("admin").runCommand({replSetGetStatus: 1});
assert.commandWorked(primaryStatus, "replSetGetStatus failed.");
assert.eq(primaryStatus.myState, ReplSetTest.State.PRIMARY, "Expected original primary node to still be primary");

replTest.stopSet();
