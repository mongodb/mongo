/**
 * When new members are added to the set with 'votes:1', we rewrite the replset config to have
 * 'newlyAdded=true' set for those nodes. When the primary learns of such a member completing
 * initial sync (via heartbeats), it initiates a reconfig to remove the corresponding 'newlyAdded'
 * field.
 *
 * TODO(SERVER-46592): This test is multiversion-incompatible in 4.6.  If we use 'requires_fcv_46'
 *                     as the tag for that, removing 'requires_fcv_44' is sufficient.  Otherwise,
 *                     please set the appropriate tag when removing 'requires_fcv_44'
 * @tags: [requires_fcv_44, requires_fcv_46]
 */

(function() {
"use strict";

load('jstests/replsets/rslib.js');

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest({
    name: testName,
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {setParameter: {enableAutomaticReconfig: true}}
});
rst.startSet();

// TODO (SERVER-47142): Replace with initiateWithHighElectionTimeout. The automatic reconfig will
// dropAllSnapshots asynchronously, precluding waiting on a stable recovery timestamp.
const cfg = rst.getReplSetConfig();
cfg.settings = cfg.settings || {};
cfg.settings["electionTimeoutMillis"] = ReplSetTest.kForeverMillis;
rst.initiateWithAnyNodeAsPrimary(
    cfg, "replSetInitiate", {doNotWaitForStableRecoveryTimestamp: true});

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

// TODO (SERVER-46808): Move this into ReplSetTest.initiate
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 0);
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1);
waitForConfigReplication(primary, rst.nodes);

assert.commandWorked(primaryColl.insert({"starting": "doc"}, {writeConcern: {w: 2}}));

jsTestLog("Adding a new node to the replica set");

const secondary = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        'failpoint.forceSyncSourceCandidate':
            tojson({mode: 'alwaysOn', data: {"hostAndPort": primary.host}}),
        'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
        'enableAutomaticReconfig': true,
    }
});

rst.reInitiate();

assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Checking for 'newlyAdded' field (should be set)");
assert(isMemberNewlyAdded(primary, 2));
let status = assert.commandWorked(primaryDb.adminCommand({replSetGetStatus: 1}));
assert.eq(status["votingMembersCount"], 2, tojson(status));
assert.eq(status["majorityVoteCount"], 2, tojson(status));
assert.eq(status["writableVotingMembersCount"], 2, tojson(status));
assert.eq(status["writeMajorityCount"], 2, tojson(status));

jsTestLog("Waiting for initial sync to complete");
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);

jsTestLog("Checking for 'newlyAdded' field (should have been removed)");
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 2);
status = assert.commandWorked(primaryDb.adminCommand({replSetGetStatus: 1}));
assert.eq(status["votingMembersCount"], 3, tojson(status));
assert.eq(status["majorityVoteCount"], 2, tojson(status));
assert.eq(status["writableVotingMembersCount"], 3, tojson(status));
assert.eq(status["writeMajorityCount"], 2, tojson(status));

jsTestLog("Making sure the set can accept w:3 writes");
assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 3}}));

rst.stopSet();
})();