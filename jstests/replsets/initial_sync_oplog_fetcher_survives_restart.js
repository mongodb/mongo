/**
 * Tests that initial sync survives a restart during the oplog fetching process.
 * @tags: [
 *   requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const testName = "initial_sync_oplog_fetcher_survives_restart";
const rst = new ReplSetTest({name: testName, nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const primaryDb = primary.getDB("test");

jsTest.log("Adding a new node to the replica set");
const secondary = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        // Wait for the cloners to finish.
        'failpoint.initialSyncHangAfterDataCloning': tojson({mode: 'alwaysOn'}),
        // Pause the oplog fetcher so we can run it after the cloners finish, to test
        // it's restart behavior in isolation.
        'failpoint.hangBeforeStartingOplogFetcher': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
    }
});
rst.reInitiate();
const nRetries = 2;

jsTestLog("Waiting for cloning to complete.");
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangAfterDataCloning",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Add some data to the primary so there are oplog entries to fetch.
assert.commandWorked(primaryDb.test.insert([{a: 1}, {b: 2}, {c: 3}]));

jsTestLog("Testing restart of sync source in oplog fetcher.");

// We stop the node and wait for it, then start it separately, to avoid the initial sync completing
// before the node actually stops.
rst.stop(primary, null, null, {forRestart: true, waitPid: true});

jsTestLog("Releasing the oplog fetcher failpoint.");
assert.commandWorked(secondary.getDB("test").adminCommand(
    {configureFailPoint: "hangBeforeStartingOplogFetcher", mode: "off"}));

// Wait for retries to happen while the sync source is down.
checkLog.containsWithAtLeastCount(secondary, "OplogFetcher reconnecting", nRetries);

const options = {
    waitForConnect: true
};

primary = rst.start(primary, options, true /* restart */);

jsTestLog("Releasing the after data cloning failpoint.");
assert.commandWorked(secondary.getDB("test").adminCommand(
    {configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));

jsTestLog("Waiting for initial sync to complete.");
// Wait for initial sync to complete.
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);
rst.stopSet();
})();
