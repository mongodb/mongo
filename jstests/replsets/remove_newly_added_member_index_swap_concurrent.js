/**
 * Tests running a reconfig in the window where an automatic reconfig has been scheduled, but not
 * yet executed. The externally-issued reconfig swaps member positions (indexes) in the config so
 * that we can test that we target members by id and not index when removing 'newlyAdded' fields.
 *
 * @tags: [
 *   requires_fcv_46,
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
    nodeOptions: {setParameter: {enableAutomaticReconfig: true}},
    settings: {chainingAllowed: false}
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

// TODO (SERVER-46808): Move this into ReplSetTest.initiate
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 0);
waitForConfigReplication(primary, rst.nodes);

assert.commandWorked(primaryColl.insert({"starting": "doc"}));

const newNodeOne = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
        'enableAutomaticReconfig': true,
    }
});

const newNodeTwo = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
        'enableAutomaticReconfig': true,
    }
});

rst.reInitiate();

jsTestName("Checking that the member ids and indexes match in the current config");
let configOnDisk = primary.getDB("local").system.replset.findOne();
assert.eq(1, configOnDisk.members[1]._id, configOnDisk);
assert.eq(2, configOnDisk.members[2]._id, configOnDisk);

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

jsTestLog("Allowing primary to initiate the 'newlyAdded' field removal for the first node");
let hangDuringAutomaticReconfigFP = configureFailPoint(primaryDb, "hangDuringAutomaticReconfig");
assert.commandWorked(
    newNodeOne.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.waitForState(newNodeOne, ReplSetTest.State.SECONDARY);

hangDuringAutomaticReconfigFP.wait();

jsTestLog("Swapping members in the config");
const config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
const tempMemberOne = Object.assign({}, config.members[1]);
config.members[1] = config.members[2];
config.members[2] = tempMemberOne;
config.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
waitForConfigReplication(primary);

jsTestLog("Making sure the config now has the ids and indexes flipped");
configOnDisk = primary.getDB("local").system.replset.findOne();
assert.eq(2, configOnDisk.members[1]._id, configOnDisk);
assert.eq(1, configOnDisk.members[2]._id, configOnDisk);

jsTestLog("Proceeding with the original 'newlyAdded' removal (_id 1, index 2)");
hangDuringAutomaticReconfigFP.off();
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 2 /* memberIndex */);

jsTestLog("Verifying the results of the 'newlyAdded' removal");
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

jsTestLog("Making sure the other node still has its 'newlyAdded' field");
assert(isMemberNewlyAdded(primary, 1));
configOnDisk = primary.getDB("local").system.replset.findOne();
assert.eq(2, configOnDisk.members[1]._id, configOnDisk);
assert(configOnDisk.members[1]["newlyAdded"], configOnDisk);

jsTestLog("Letting the other secondary node finish initial sync");
assert.commandWorked(
    newNodeTwo.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.waitForState(newNodeTwo, ReplSetTest.State.SECONDARY);

jsTestLog("Waiting for the second 'newlyAdded' field to be removed (_id 2, index 1)");
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1 /* memberIndex */);

jsTestLog("Verifying the results of the second 'newlyAdded' removal");
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