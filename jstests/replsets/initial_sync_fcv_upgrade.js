/*
 * Tests that on a successful fcv upgrade, adding new voters to the replica set
 * makes replConfig to contain members with 'newlyAdded' field.
 *
 * This tests behavior centered around FCV upgrade.
 * @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

load('jstests/replsets/rslib.js');

// Start a single node replica set.
// Disable Chaining so that initial sync nodes always sync from primary.
const rst = new ReplSetTest({nodes: 1, settings: {chainingAllowed: false}});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const dbName = jsTest.name();
const collName = "coll";

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const primaryAdminDB = primary.getDB("admin");

// Set FCV to 4.4 to test the upgrade behavior.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandWorked(primary.adminCommand({clearLog: "global"}));

let startupParams = {};
startupParams['failpoint.initialSyncHangAfterDataCloning'] = tojson({mode: 'alwaysOn'});

jsTestLog("Upgrade FCV to " + latestFCV);
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

jsTestLog("Adding a new voting node (node1) to the replica set");
const node1 = rst.add({rsConfig: {priority: 0, votes: 1}, setParameter: startupParams});
rst.reInitiate();

jsTestLog("Wait for node1 to hang during initial sync");
checkLog.containsJson(node1, 21184);

// Check that 'newlyAdded' field is  set.
assert(isMemberNewlyAdded(primary, 1));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 2,
});

jsTestLog("Perform test cleanup");
assert.commandWorked(
    node1.adminCommand({configureFailPoint: "initialSyncHangAfterDataCloning", mode: 'off'}));
rst.waitForState(node1, ReplSetTest.State.SECONDARY);

// Check if writeConcern: 2 works.
assert.commandWorked(primaryColl.insert({_id: 0}, {"writeConcern": {"w": 2}}));

rst.stopSet();
}());