/*
 * Tests that an arbiter will always be on the latest FCV regardless of the FCV of the replica set.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

function runTest(FCV) {
    let rst = new ReplSetTest({nodes: [{}, {rsConfig: {arbiterOnly: true}}]});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    if (FCV != latestFCV) {
        assert.commandWorked(primary.getDB("admin").runCommand(
            {setFeatureCompatibilityVersion: FCV, confirm: true}));
    }

    const primaryFCV = assert.commandWorked(
        primary.getDB("admin").runCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(primaryFCV.featureCompatibilityVersion.version, FCV, tojson(primaryFCV));

    // The arbiter should always have an FCV matching kLatest.
    const arbiter = rst.getArbiter();
    const arbiterFCV = assert.commandWorked(
        arbiter.getDB("admin").runCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(arbiterFCV.featureCompatibilityVersion.version, latestFCV, tojson(arbiterFCV));
    rst.stopSet();
}

runTest(latestFCV);
runTest(lastLTSFCV);
if (lastContinuousFCV != lastLTSFCV) {
    runTest(lastContinuousFCV);
}