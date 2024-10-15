/**
 * Tests that initial sync restarts on the same collection if there is a network error during the
 * collStats command (rather than restarting initial sync).
 */

import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Create set with one node.
const rst = new ReplSetTest({name: jsTestName(), nodes: 1, useBridge: true});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
const primaryColl = primaryDb.getCollection('testColl');

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Add some data to be cloned.
assert.commandWorked(
    primaryDb.testColl.insert([{a: 1}, {b: 2}, {c: 3}, {d: 4}, {e: 5}, {f: 6}, {g: 7}]));

jsTest.log("Adding a new node to the replica set");
// Create the secondary node which will use logical initial sync to sync from the primary.
// Enable hangBeforeClonerStage and hangBeforeRetryingClonerStage so we can test that if the
// collStats network command fails, only this stage is retried until it succeeds. Set
// numInitialSyncAttempts to 1 to confirm that it doesn't retry the entire initial sync if the
// command fails.
const secondary = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        // This test is specifically testing that the cloners stop, so we turn off the
        // oplog fetcher to ensure that we don't inadvertently test that instead.
        'failpoint.hangBeforeStartingOplogFetcher': tojson({mode: 'alwaysOn'}),
        'failpoint.hangBeforeClonerStage': tojson({
            mode: 'alwaysOn',
            data: {cloner: "CollectionCloner", stage: "collStats", nss: primaryColl.getFullName()}
        }),
        'failpoint.hangBeforeRetryingClonerStage': tojson({
            mode: 'alwaysOn',
            data: {cloner: "CollectionCloner", stage: "collStats", nss: primaryColl.getFullName()}
        }),
        'failpoint.hangAfterClonerStage': tojson({
            mode: 'alwaysOn',
            data: {cloner: "CollectionCloner", stage: "collStats", nss: primaryColl.getFullName()}
        }),
        'numInitialSyncAttempts': 1,
    }
});
rst.reInitiate();
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: 'hangBeforeClonerStage',
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Disconnect the secondary from the primary to get a network error on the first collStats command.
primary.disconnect(secondary);
jsTestLog("Secondary disconnected; Allowing secondary initial sync to resume.");
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "hangBeforeClonerStage", mode: 'off'}));

// Verify the node retries the collStats stage.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: 'hangBeforeRetryingClonerStage',
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

primary.reconnect(secondary);

// Turn off the failpoint before retrying collStats and allow the node to proceed with initial sync.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "hangBeforeRetryingClonerStage", mode: 'off'}));

// Wait for the failpoint after collStats stage.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: 'hangAfterClonerStage',
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Verify that the collStats command worked.
const testDbRes = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
const testCollRes = testDbRes.initialSyncStatus.databases.test["test.testColl"];

assert.eq(testCollRes.bytesToCopy, 231);

// Turn off the failpoint after collStats stage and allow node to proceed with initial sync.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "hangAfterClonerStage", mode: 'off'}));

// Let the oplog fetcher run so we can complete initial sync.
jsTestLog("Releasing the oplog fetcher failpoint.");
assert.commandWorked(secondary.getDB("test").adminCommand(
    {configureFailPoint: "hangBeforeStartingOplogFetcher", mode: "off"}));

jsTestLog("Waiting for initial sync to complete.");
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);

rst.stopSet();
