/**
 * Tests the behavior of the 'runFeatureFlagMultiversionTest' helper.
 * This test requires that the featureFlagToaster and featureFlagSpoon parameters to be enabled and
 * the featureFlagFryer parameter to be disabled.
 *
 * Add test tags for the feature flags depended on by this test.
 * @tags: [featureFlagToaster, featureFlagSpoon]
 */

(function() {
'use strict';

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
    assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));
    checkFCV(adminDB, downgradeFCV);
    if (downgradeFCV === lastLTSFCV) {
        numLastLTSRuns++;
    } else {
        numLastContRuns++;
    }
    rst.stopSet();
}

try {
    // We expect the test run to fail when using a non-existent feature flag.
    runFeatureFlagMultiversionTest("nonExistentFeatureFlag", runTest);
} catch (error) {
}

// No tests should have been run when a non-existent feature flag is passed in.
assert.eq(numLastLTSRuns, 0);
assert.eq(numLastContRuns, 0);

// No tests should have been run when a disabled feature flag is passed in.
runFeatureFlagMultiversionTest("featureFlagFryer", runTest);
assert.eq(numLastLTSRuns, 0);
assert.eq(numLastContRuns, 0);

// Pass in a feature flag that is slated for release in the latest version. This should run against
// both the last-lts and last-continuous FCV.
runFeatureFlagMultiversionTest("featureFlagToaster", runTest);
assert.eq(numLastLTSRuns, 1);
assert.eq(numLastContRuns, 1);

// Pass in a feature flag that was released in an older version. This should only run against the
// last-lts FCV.
runFeatureFlagMultiversionTest("featureFlagSpoon", runTest);
assert.eq(numLastLTSRuns, 2);
assert.eq(numLastContRuns, 1);
})();