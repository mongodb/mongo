/**
 * This test sets up a node to have a 'newlyAdded' field, then lets it exit initial sync while
 * racing with a user initiated reconfig. The test verifies that there are no adverse effects (other
 * than one party having to retry).
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

const baseConfig = rst.getReplSetConfigFromNode();

jsTestLog("Doing a reconfig while exiting initial sync");

const userReconfigFn = function() {
    const sleepAmount = Math.floor(Math.random() * 3000);  // 0-3000 ms
    print("Sleeping for " + sleepAmount + " milliseconds");
    sleep(sleepAmount);
    const res = rs.add({priority: 0, votes: 0, host: "abcde:12345"});
    assert.neq("", res);
    assert.commandWorked(res);
};
const waitForUserReconfig = startParallelShell(userReconfigFn, primary.port);

assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
waitForUserReconfig();

jsTestLog("Waiting for 'newlyAdded' field to be removed");
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1);
assertVoteCount(primary, {
    votingMembersCount: 2,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 3,
});

jsTestLog("Making sure we can see the results of the user reconfig");
const modifiedConfig = rst.getReplSetConfigFromNode();
assert.eq(3, modifiedConfig.members.length, () => [tojson(baseConfig), tojson(modifiedConfig)]);
assert.eq("abcde:12345",
          modifiedConfig.members[2].host,
          () => [tojson(baseConfig), tojson(modifiedConfig)]);

jsTestLog("Making sure set can accept w:2 writes");
assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 2}}));

rst.awaitReplication();
rst.stopSet();
})();