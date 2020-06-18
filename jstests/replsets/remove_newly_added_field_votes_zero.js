/**
 * Test that the 'newlyAdded' field of a MemberConfig is not added for nodes configured with
 * 'votes:0', but is also removed if a node ends up with 'votes:0' and 'newlyAdded'.
 *
 * @tags: [
 *   requires_fcv_46,
 * ]
 */

(function() {
"use strict";

load('jstests/replsets/rslib.js');
load("jstests/libs/fail_point_util.js");

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest(
    {name: testName, nodes: 1, nodeOptions: {setParameter: {enableAutomaticReconfig: true}}});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

// TODO(SERVER-46808): Move this into ReplSetTest.initiate
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 0);
waitForConfigReplication(primary, rst.nodes);

assert.commandWorked(primaryColl.insert({a: 1}));

jsTestLog("Adding a new non-voting node to the replica set");
const secondary0 = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
        'enableAutomaticReconfig': true,
    }
});
rst.reInitiate();

assert.commandWorked(secondary0.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Checking that 'newlyAdded' field is not set");
assert(!isMemberNewlyAdded(primary, 1));
assert.eq(0, rst.getReplSetConfigFromNode(primary.nodeId).members[1].votes);
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 2,
});

jsTestLog("Waiting for initial sync to complete");
assert.commandWorked(
    secondary0.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.waitForState(secondary0, ReplSetTest.State.SECONDARY);

jsTestLog("Checking that 'newlyAdded' field is still not set");
assert(!isMemberNewlyAdded(primary, 1));
assert.eq(0, rst.getReplSetConfigFromNode(primary.nodeId).members[1].votes);
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 2,
});

jsTestLog("Making sure the set can accept w:2 writes");
assert.commandWorked(primaryColl.insert({a: 2}, {writeConcern: {w: 2}}));

jsTestLog("Adding a new voting node to the replica set");
const secondary1 = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
        'enableAutomaticReconfig': true,
    }
});
rst.reInitiate();

assert.commandWorked(secondary1.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Checking that 'newlyAdded' field is set");
assert.eq(0, rst.getReplSetConfigFromNode(primary.nodeId).members[1].votes);
assert.eq(1, rst.getReplSetConfigFromNode(primary.nodeId).members[2].votes);
assert(!isMemberNewlyAdded(primary, 1));
assert(isMemberNewlyAdded(primary, 2));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3,
});

jsTestLog("Reconfiguring new node to have 0 votes");
let cfg = rst.getReplSetConfigFromNode(primary.nodeId);
cfg.version += 1;
cfg.members[2].votes = 0;
assert.commandWorked(
    primary.adminCommand({replSetReconfig: cfg, maxTimeMS: ReplSetTest.kDefaultTimeoutMS}));

assert.eq(0, rst.getReplSetConfigFromNode(primary.nodeId).members[1].votes);
assert.eq(0, rst.getReplSetConfigFromNode(primary.nodeId).members[2].votes);
assert(!isMemberNewlyAdded(primary, 1));
assert(isMemberNewlyAdded(primary, 2));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3,
});

jsTestLog("Waiting for second initial sync to complete");
assert.commandWorked(
    secondary1.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.waitForState(secondary1, ReplSetTest.State.SECONDARY);

jsTestLog("Checking that 'newlyAdded' field was removed");
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 2);
assert.eq(0, rst.getReplSetConfigFromNode(primary.nodeId).members[1].votes);
assert.eq(0, rst.getReplSetConfigFromNode(primary.nodeId).members[2].votes);
assert(!isMemberNewlyAdded(primary, 1));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3,
});

jsTestLog("Making sure the set can accept w:3 writes");
assert.commandWorked(primaryColl.insert({a: 3}, {writeConcern: {w: 3}}));

rst.stopSet();
})();