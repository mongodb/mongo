/**
 * Tests that an initial syncing node being forced to sync from the primary through
 * unsupportedSyncSource will be able to bypass an initialSyncSourceReadPreference
 * of secondary only.
 *
 * @tags: [
 *  requires_fcv_82
 * ]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({name: jsTestName(), nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("testDb");

// Add some data to be cloned.
const primaryColl = primaryDb.getCollection("testColl");
assert.commandWorked(primaryColl.insert([{a: 1}, {b: 2}, {c: 3}]));

rst.awaitReplication();

jsTestLog(
    "Adding new node with unsupportedSyncSource to be the primary and readPreference to be secondary only.");

const initialSyncNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'unsupportedSyncSource': primary.host,
        'initialSyncSourceReadPreference': 'secondary',
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
    }
});
rst.reInitiate();

jsTestLog("Wait until after the sync source has been chosen to confirm it is the primary.");

assert.commandWorked(initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCopyingDatabases",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
assert.eq(primary.host, res.syncSourceHost);

jsTestLog("Sync source confirmed to be primary.");

// Allow the node to continue copying databases.
assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

rst.awaitSecondaryNodes();

rst.stopSet();
