/**
 * When new members are added to the set with 'votes:1', we rewrite the replset config to have
 * 'newlyAdded=true' set for those nodes. When the primary learns of such a member completing
 * initial sync (via heartbeats), it initiates a reconfig to remove the corresponding 'newlyAdded'
 * field.
 *
 * @tags: [
 * ]
 */

import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkWriteConcernTimedOut} from "jstests/libs/write_concern_util.js";
import {
    assertVoteCount,
    getConfigWithNewlyAdded,
    isMemberNewlyAdded,
    waitForNewlyAddedRemovalForNodeToBeCommitted,
} from "jstests/replsets/rslib.js";

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest({
    name: testName,
    nodes: [{}, {}, {rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false},
    useBridge: true,
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

// We did two automatic reconfigs to remove 'newlyAdded' fields (for members 1 and 2).
const replMetricsAtStart = primaryDb.serverStatus().metrics.repl;
assert(replMetricsAtStart.hasOwnProperty("reconfig"));
const numAutoReconfigsAtStart = replMetricsAtStart.reconfig.numAutoReconfigsForRemovalOfNewlyAddedFields;
// We did two automatic reconfigs while setting up the original replset.
assert.eq(2, numAutoReconfigsAtStart, replMetricsAtStart);

assert.commandWorked(primaryColl.insert({"starting": "doc"}, {writeConcern: {w: 3}}));

jsTestLog("Adding a new node to the replica set");
const secondary = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        "failpoint.initialSyncHangBeforeFinish": tojson({mode: "alwaysOn"}),
        "numInitialSyncAttempts": 1,
    },
});
rst.reInitiate();
assert.commandWorked(
    secondary.adminCommand({
        waitForFailPoint: "initialSyncHangBeforeFinish",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

jsTestLog("Checking for 'newlyAdded' field (should be set)");
assert(isMemberNewlyAdded(primary, 3));

jsTestLog("Making sure the 'newlyAdded' field is not visible in replSetGetConfig");
let getConfigRes = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
let newNodeRes = getConfigRes.members[3];
assert.eq(false, newNodeRes.hasOwnProperty("newlyAdded"), getConfigRes);

jsTestLog("Making sure the 'newlyAdded' field is visible in replSetGetConfig with test param");
getConfigRes = getConfigWithNewlyAdded(primary).config;
newNodeRes = getConfigRes.members[3];
assert.eq(true, newNodeRes.hasOwnProperty("newlyAdded"), getConfigRes);

jsTestLog("Checking behavior with 'newlyAdded' field set, during initial sync");
assertVoteCount(primary, {
    votingMembersCount: 3,
    majorityVoteCount: 2,
    writableVotingMembersCount: 3,
    writeMajorityCount: 2,
    totalMembersCount: 4,
});
assert.commandWorked(primaryColl.insert({a: 0}, {writeConcern: {w: 3}}));
assert.commandWorked(primaryColl.insert({a: 1}, {writeConcern: {w: "majority"}}));

// Initial syncing nodes do not acknowledge replication.
let res = primaryDb.runCommand({insert: collName, documents: [{a: 2}], writeConcern: {w: 4, wtimeout: 1000}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
checkWriteConcernTimedOut(res);

// Wait for the new config to be replicated before disconnecting the secondary.
rst.waitForConfigReplication(primary);

// Only two nodes are needed for majority (0 and 1).
rst.nodes[2].disconnect(rst.nodes);
assert.commandWorked(primaryColl.insert({a: 3}, {writeConcern: {w: "majority"}}));

function stepUpNode(rst, newPrimary, liveSecondaries) {
    rst.awaitReplication(null, null, liveSecondaries);
    assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
    assert.eq(rst.getPrimary(), newPrimary);
    // Waiting for the background step-up writes here means they won't interfere with the next
    // step-up.  We await their replication as part of the awaitReplication before stepping up.
    rst.waitForStepUpWrites(newPrimary);
}

// Only two nodes are needed for an election (0 and 1).
stepUpNode(rst, rst.nodes[1], [rst.nodes[1]]);
rst.waitForConfigReplication(rst.nodes[1], [rst.nodes[0], rst.nodes[1], rst.nodes[3]]);

// Reset node 0 to be primary.
stepUpNode(rst, rst.nodes[0], [rst.nodes[0], rst.nodes[1]]);
rst.waitForConfigReplication(rst.nodes[0], [rst.nodes[0], rst.nodes[1], rst.nodes[3]]);

// Initial syncing nodes do not acknowledge replication.
rst.nodes[1].disconnect(rst.nodes);
res = primaryDb.runCommand({insert: collName, documents: [{a: 2}], writeConcern: {w: "majority", wtimeout: 1000}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
checkWriteConcernTimedOut(res);

// 'newlyAdded' nodes don't vote.
rst.nodes[1].reconnect(rst.nodes[3]);
assert.commandFailedWithCode(rst.nodes[1].adminCommand({replSetStepUp: 1}), ErrorCodes.CommandFailed);
rst.nodes[1].reconnect(rst.nodes);
rst.nodes[2].reconnect(rst.nodes);

jsTestLog("Waiting for initial sync to complete");
let doNotRemoveNewlyAddedFP = configureFailPoint(primaryDb, "doNotRemoveNewlyAddedOnHeartbeats");
assert.commandWorked(secondary.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.awaitSecondaryNodes(null, [secondary]);

jsTestLog("Checking that the 'newlyAdded' field is still set");
assert(isMemberNewlyAdded(primary, 3));

jsTestLog("Making sure the 'newlyAdded' field is still not visible in replSetGetConfig");
getConfigRes = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
newNodeRes = getConfigRes.members[3];
assert.eq(false, newNodeRes.hasOwnProperty("newlyAdded"), getConfigRes);

jsTestLog("Checking behavior with 'newlyAdded' field set, after initial sync");
assertVoteCount(primary, {
    votingMembersCount: 3,
    majorityVoteCount: 2,
    writableVotingMembersCount: 3,
    writeMajorityCount: 2,
    totalMembersCount: 4,
});

// Voting isn't required for satisfying numerical write concerns.
assert.commandWorked(primaryColl.insert({a: 4}, {writeConcern: {w: 4}}));

// Only two nodes are needed to satisfy w:majority (0 and 1).
rst.nodes[2].disconnect(rst.nodes);
rst.nodes[3].disconnect(rst.nodes);
assert.commandWorked(primaryColl.insert({a: 6}, {writeConcern: {w: "majority"}}));

// Only two nodes are needed for an election (0 and 1).
stepUpNode(rst, rst.nodes[1], [rst.nodes[1]]);
rst.waitForConfigReplication(rst.nodes[1], [rst.nodes[0], rst.nodes[1]]);

// Reset node 0 to be primary.
stepUpNode(rst, rst.nodes[0], [rst.nodes[0], rst.nodes[1]]);
rst.waitForConfigReplication(rst.nodes[0], [rst.nodes[0], rst.nodes[1]]);

// 'newlyAdded' nodes cannot be one of the two nodes to satisfy w:majority.
rst.nodes[3].reconnect(rst.nodes);
rst.nodes[1].disconnect(rst.nodes);
res = primaryDb.runCommand({insert: collName, documents: [{a: 7}], writeConcern: {w: "majority", wtimeout: 1000}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
checkWriteConcernTimedOut(res);
rst.nodes[1].reconnect(rst.nodes);

// 'newlyAdded' nodes don't vote.
rst.nodes[2].disconnect(rst.nodes);
rst.nodes[0].disconnect(rst.nodes);
assert.commandFailedWithCode(rst.nodes[1].adminCommand({replSetStepUp: 1}), ErrorCodes.CommandFailed);

rst.nodes[0].reconnect(rst.nodes);
rst.nodes[2].reconnect(rst.nodes);

// Record metric for number of automatic reconfigs before we perform the next one.
const replMetricsBefore = primaryDb.serverStatus().metrics.repl;
assert(replMetricsBefore.hasOwnProperty("reconfig"));
const numAutoReconfigsBefore = replMetricsBefore.reconfig.numAutoReconfigsForRemovalOfNewlyAddedFields;
// We did two automatic reconfigs while setting up the original replset.
assert.eq(2, numAutoReconfigsBefore, replMetricsBefore);

jsTestLog("Waiting for 'newlyAdded' field to be removed");
doNotRemoveNewlyAddedFP.off();
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 3);
assertVoteCount(primary, {
    votingMembersCount: 4,
    majorityVoteCount: 3,
    writableVotingMembersCount: 4,
    writeMajorityCount: 3,
    totalMembersCount: 4,
});

jsTestLog("Checking that the metric for removal of 'newlyAdded' fields was incremented");
const replMetricsAfter = primaryDb.serverStatus().metrics.repl;
assert(replMetricsAfter.hasOwnProperty("reconfig"), replMetricsAfter);
const numAutoReconfigsAfter = replMetricsAfter.reconfig.numAutoReconfigsForRemovalOfNewlyAddedFields;
assert.eq(3, numAutoReconfigsAfter, replMetricsAfter);

jsTestLog("Testing behavior during steady state");
assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 4}}));

// Only 3 nodes are needed to satisfy w:majority, and the node that was newly added (and no longer
// is) can be one of them (0, 1, and 3).
rst.nodes[2].disconnect(rst.nodes);

assert.commandWorked(primaryColl.insert({a: 8}, {writeConcern: {w: "majority"}}));

// Only three nodes are needed for an election (0, 1, and 3).
rst.waitForConfigReplication(rst.nodes[0], [rst.nodes[1]]);
stepUpNode(rst, rst.nodes[1], [rst.nodes[1]]);
rst.waitForConfigReplication(rst.nodes[1], [rst.nodes[0], rst.nodes[1], rst.nodes[3]]);

// Reset node 0 to be primary.
stepUpNode(rst, rst.nodes[0], [rst.nodes[0], rst.nodes[1]]);
rst.waitForConfigReplication(rst.nodes[0], [rst.nodes[0], rst.nodes[1], rst.nodes[3]]);

// 3 nodes are needed for a w:majority write.
rst.nodes[3].disconnect(rst.nodes);
res = primaryDb.runCommand({insert: collName, documents: [{a: 9}], writeConcern: {w: "majority", wtimeout: 1000}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
checkWriteConcernTimedOut(res);

// 3 nodes are needed to win an election
assert.commandFailedWithCode(rst.nodes[1].adminCommand({replSetStepUp: 1}), ErrorCodes.CommandFailed);

rst.nodes[2].reconnect(rst.nodes);
rst.nodes[3].reconnect(rst.nodes);

rst.stopSet();
