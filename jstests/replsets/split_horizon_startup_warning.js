/**
 * Ensures that a startup warning is issued if a node starts with an IP address in its
 * ReplSetConfig's SplitHorizon configuration.
 * @tags: [
 *   requires_persistence,
 * ]
 */

(function() {
'use strict';

// Tests that ReplSets that start with an IP address in the previous SplitHorizon configuration will
// emit a startupWarning. The warning itself should not crash the server, but
// disableSplitHorizonIPCheck is required to prevent crashing when the bad config is initially set.
function testStartupWarnings(horizonName, options = {}) {
    jsTestLog("Running startupWarning check.");
    const includesStartupWarning = (line) =>
        line.includes("Found split horizon configuration using IP");

    // Start an initial Replica Set with no SplitHorizon configuration
    const replTest = new ReplSetTest({
        nodes: 1,
        nodeOptions: Object.assign({setParameter: {disableSplitHorizonIPCheck: true}}, options)
    });
    replTest.startSet();
    replTest.initiate();
    let replPrimary = replTest.getPrimary();
    let testDB = replPrimary.getDB("test");

    // Ensure that the startup warning is not issued if the SplitHorizon configuration is normal
    let startupWarnings = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
    jsTestLog("First-time startupWarnings:\n" + tojson(startupWarnings));
    assert(!startupWarnings.log.some(includesStartupWarning));
    const rsConfig = replTest.getReplSetConfig();
    for (let i = 0; i < rsConfig.members.length; i++) {
        rsConfig.members[i].horizons = {horizon_name: horizonName};
    }
    jsTestLog(rsConfig);
    rsConfig["version"] = 2;
    assert.commandWorked(replPrimary.adminCommand({replSetReconfig: rsConfig}));

    // Restart the replset
    replTest.restart(replPrimary);
    replPrimary = replTest.getPrimary();
    testDB = replPrimary.getDB("test");

    startupWarnings = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
    jsTestLog("Post-restart startupWarnings:\n" + tojson(startupWarnings));
    assert(startupWarnings.log.some(includesStartupWarning));
    replTest.stopSet();
}

// Check for startup warnings about IP addresses in SplitHorizon mappings
testStartupWarnings("12.34.56.78");
testStartupWarnings("12.34.56.78/20");
})();
