/*
 * Tests that an arbiter will default to the latest FCV regardless of the FCV of the replica set.
 */
(function() {
"use strict";

function runTest(FCV) {
    let rst = new ReplSetTest(
        {nodes: [{}, {rsConfig: {arbiterOnly: true}}], nodeOpts: {binVersion: FCV}});
    rst.startSet();
    rst.initiate();

    const arbiter = rst.getArbiter();
    const res = assert.commandWorked(
        arbiter.getDB("admin").runCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    assert.eq(res.featureCompatibilityVersion.version, latestFCV, tojson(res));
    rst.stopSet();
}

runTest(latestFCV);
runTest(lastLTSFCV);
if (lastContinuousFCV != lastLTSFCV) {
    runTest(lastContinuousFCV);
}
}());
