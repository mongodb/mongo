/**
 * Tests that additions or removals of 'newlyAdded' fields do not interfere with user initiated
 * reconfigs.
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
    settings: {chainingAllowed: false},
    useBridge: true
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

jsTestLog("Adding a new node to the replica set");
const secondary = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
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

jsTestLog("Checking that the 'newlyAdded' field is set on the new node");
assert(isMemberNewlyAdded(primary, 1));

jsTestLog("Checking vote counts while the secondary is in initial sync");
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 2
});

jsTestLog("Checking that user reconfigs still succeed, during initial sync");
let config = rst.getReplSetConfigFromNode();

// First try making a change without modifying the member set.
jsTestLog("[1] Change in config.settings, during initial sync");
const baseElectionTimeoutMillis = config.settings.electionTimeoutMillis;
config.settings.electionTimeoutMillis++;
reconfig(rst, config, false /* force */, true /* doNotWaitForMembers */);

config = rst.getReplSetConfigFromNode();
assert.eq(baseElectionTimeoutMillis + 1, config.settings.electionTimeoutMillis);

// Check 'newlyAdded' and vote counts.
assert(isMemberNewlyAdded(primary, 1));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 2
});

// Now try adding a member.
jsTestLog("[2] Member addition, during initial sync");
rst.add({rsConfig: {priority: 0, votes: 0}});
rst.reInitiate();

// Check 'newlyAdded' and vote counts.
assert(isMemberNewlyAdded(primary, 1));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3
});

// Do not let 'newlyAdded' be removed yet.
jsTestLog("Allowing member to complete initial sync");

let doNotRemoveNewlyAddedFP = configureFailPoint(primaryDb, "doNotRemoveNewlyAddedOnHeartbeats");
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);

jsTestLog("Checking that the 'newlyAdded' field is still set");
assert(isMemberNewlyAdded(primary, 1));

jsTestLog("Checking behavior with 'newlyAdded' field set, after initial sync");
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3,
});

jsTestLog("Checking that user reconfigs still succeed, after initial sync");
config = rst.getReplSetConfigFromNode();

jsTestLog("[3] Change in config.settings, during initial sync");
config.settings.electionTimeoutMillis++;
reconfig(rst, config, false /* force */, true /* doNotWaitForMembers */);

config = rst.getReplSetConfigFromNode();
assert.eq(baseElectionTimeoutMillis + 2, config.settings.electionTimeoutMillis);

// Check 'newlyAdded' and vote counts.
assert(isMemberNewlyAdded(primary, 1));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3
});

// Now try removing the member we added above.
jsTestLog("[4] Member removal, after initial sync");
config = rst.getReplSetConfigFromNode();
const twoNodeConfig = Object.assign({}, config);
twoNodeConfig.members = twoNodeConfig.members.slice(0, 2);  // Remove the last node.
reconfig(rst, twoNodeConfig);
rst.remove(2);

// Check 'newlyAdded' and vote counts.
assert(isMemberNewlyAdded(primary, 1));
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 2
});

jsTestLog("Waiting for 'newlyAdded' field to be removed");
doNotRemoveNewlyAddedFP.off();
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1);
assertVoteCount(primary, {
    votingMembersCount: 2,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 2,
});

jsTestLog("Making sure set can accept w:2 writes");
assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 2}}));

rst.awaitReplication();
rst.stopSet();
})();