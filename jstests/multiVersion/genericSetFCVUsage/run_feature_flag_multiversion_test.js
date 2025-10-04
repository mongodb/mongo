/**
 * Tests the behavior of the 'runFeatureFlagMultiversionTest' helper.
 * This test requires that the featureFlagToaster and featureFlagSpoon parameters to be enabled and
 * the featureFlagFryer parameter to be disabled.
 *
 * Add test tags for the feature flags depended on by this test.
 * @tags: [featureFlagToaster, featureFlagSpoon]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let numLastLTSRuns = 0;
let numLastContRuns = 0;

function runTest(downgradeFCV) {
    let name = "feature_flag_test";
    let nodes = {n0: {binVersion: "latest"}};
    let rst = new ReplSetTest({name: name, nodes: nodes});
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    let adminDB = primary.getDB("admin");
    assert.commandWorked(primary.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));
    assert.commandFailedWithCode(
        adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}),
        549181,
    );
    checkFCV(adminDB, downgradeFCV, downgradeFCV);
    if (downgradeFCV === lastLTSFCV) {
        numLastLTSRuns++;
    }
    if (downgradeFCV === lastContinuousFCV) {
        numLastContRuns++;
    }
    rst.stopSet();
}

try {
    // We expect the test run to fail when using a non-existent feature flag.
    runFeatureFlagMultiversionTest("nonExistentFeatureFlag", runTest);
} catch (error) {}

// No tests should have been run when a non-existent feature flag is passed in.
assert.eq(numLastLTSRuns, 0);
assert.eq(numLastContRuns, 0);

// No tests should have been run when a disabled feature flag is passed in.
runFeatureFlagMultiversionTest("featureFlagFryer", runTest);
assert.eq(numLastLTSRuns, 0);
assert.eq(numLastContRuns, 0);

// The counters for both lastLTS and lastContinuous runs will be incremented when the two FCVs are
// equal.
jsTestLog(`The lastLTSFCV and lastContinousFCV are equal: ${lastLTSFCV === lastContinuousFCV}`);

// Pass in a feature flag that is slated for release in the latest version. This should run against
// both the last-lts and last-continuous FCV.
runFeatureFlagMultiversionTest("featureFlagToaster", runTest);
if (lastLTSFCV === lastContinuousFCV) {
    assert.eq(numLastLTSRuns, 2);
    assert.eq(numLastContRuns, 2);
} else {
    assert.eq(numLastLTSRuns, 1);
    assert.eq(numLastContRuns, 1);
}

// Pass in a feature flag that was released in an older version. This should only run against the
// last-lts FCV.
runFeatureFlagMultiversionTest("featureFlagSpoon", runTest);
if (lastLTSFCV === lastContinuousFCV) {
    assert.eq(numLastLTSRuns, 3);
    assert.eq(numLastContRuns, 3);
} else {
    assert.eq(numLastLTSRuns, 2);
    assert.eq(numLastContRuns, 1);
}
