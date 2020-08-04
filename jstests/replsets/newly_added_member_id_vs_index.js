/**
 * Test that removal of 'newlyAdded' fields is done by memberId, not memberIndex. We do this by
 * setting up two 'newlyAdded' nodes with inverse {memberId, memberIndex} pairs and checking that we
 * remove the 'newlyAdded' field for the node identified by _id.
 *
 * @tags: [
 *   requires_fcv_47,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load('jstests/replsets/rslib.js');

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest({
    name: testName,
    nodes: 1,
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

assert.commandWorked(primaryColl.insert({"starting": "doc"}));

jsTestName("Adding two members with alternating memberIds and memberIndex fields");

// This node will be at index 1 with _id 2.
const newNodeOne = rst.add({
    rsConfig: {_id: 2, priority: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
    }
});

// This node will be at index 2 with _id 1.
const newNodeTwo = rst.add({
    rsConfig: {_id: 1, priority: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
    }
});

rst.reInitiate();

jsTestName("Checking that the config has the ids and indexes flipped");
let configOnDisk = primary.getDB("local").system.replset.findOne();
assert.eq(2, configOnDisk.members[1]._id, configOnDisk);
assert.eq(1, configOnDisk.members[2]._id, configOnDisk);

assert.commandWorked(newNodeOne.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));
assert.commandWorked(newNodeTwo.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Checking that the 'newlyAdded' field is set on both new nodes");
assert(isMemberNewlyAdded(primary, 1));
assert(isMemberNewlyAdded(primary, 2));

jsTestLog("Checking vote counts while secondaries are still in initial sync");
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3
});

jsTestName("Allowing one of the secondaries to complete initial sync (_id 1, index 2)");
assert.commandWorked(
    newNodeTwo.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.waitForState(newNodeTwo, ReplSetTest.State.SECONDARY);

jsTestName("Waiting for its 'newlyAdded' to be removed");
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 2 /* memberIndex */);

jsTestName("Vefirying the results of the 'newlyAdded' removal");
configOnDisk = primary.getDB("local").system.replset.findOne();
assert.eq(1, configOnDisk.members[2]._id, configOnDisk);
assert.eq(false, configOnDisk.members[2].hasOwnProperty("newlyAdded"), configOnDisk);
assertVoteCount(primary, {
    votingMembersCount: 2,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 3,
});

jsTestName("Making sure the other node still has its 'newlyAdded' field");
assert(isMemberNewlyAdded(primary, 1));
configOnDisk = primary.getDB("local").system.replset.findOne();
assert.eq(2, configOnDisk.members[1]._id, configOnDisk);
assert(configOnDisk.members[1]["newlyAdded"], configOnDisk);

jsTestName("Letting the other secondary node finish initial sync (_id 2, index 1)");
assert.commandWorked(
    newNodeOne.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.waitForState(newNodeOne, ReplSetTest.State.SECONDARY);

jsTestName("Waiting for the second 'newlyAdded' field to be removed.");
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1 /* memberIndex */);

jsTestName("Verifying the results of the second 'newlyAdded' removal");
configOnDisk = primary.getDB("local").system.replset.findOne();
assert.eq(2, configOnDisk.members[1]._id, configOnDisk);
assert.eq(false, configOnDisk.members[1].hasOwnProperty("newlyAdded"), configOnDisk);
assertVoteCount(primary, {
    votingMembersCount: 3,
    majorityVoteCount: 2,
    writableVotingMembersCount: 3,
    writeMajorityCount: 2,
    totalMembersCount: 3,
});

jsTestLog("Making sure set can accept w:3 writes");
assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 3}}));

rst.awaitReplication();
rst.stopSet();
})();
