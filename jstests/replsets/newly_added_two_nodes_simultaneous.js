/**
 * Tests adding two non-voting nodes to the replica set at the same time.
 *
 * @tags: [
 * ]
 */

import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    assertVoteCount,
    isMemberNewlyAdded,
    waitForNewlyAddedRemovalForNodeToBeCommitted,
} from "jstests/replsets/rslib.js";

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest({name: testName, nodes: 1, settings: {chainingAllowed: false}, useBridge: true});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

assert.commandWorked(primaryColl.insert({"starting": "doc"}));

const newNodeOne = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        "failpoint.initialSyncHangBeforeFinish": tojson({mode: "alwaysOn"}),
        "numInitialSyncAttempts": 1,
    },
});

const newNodeTwo = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        "failpoint.initialSyncHangBeforeFinish": tojson({mode: "alwaysOn"}),
        "numInitialSyncAttempts": 1,
    },
});

rst.reInitiate();
assert.commandWorked(
    newNodeOne.adminCommand({
        waitForFailPoint: "initialSyncHangBeforeFinish",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);
assert.commandWorked(
    newNodeTwo.adminCommand({
        waitForFailPoint: "initialSyncHangBeforeFinish",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

jsTestLog("Checking that the 'newlyAdded' field is set on both new nodes");
assert(isMemberNewlyAdded(primary, 1));
assert(isMemberNewlyAdded(primary, 2));

jsTestLog("Checking vote counts while secondaries are still in initial sync");
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3,
});

jsTestLog("Allowing secondaries to complete initial sync");
assert.commandWorked(newNodeOne.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
assert.commandWorked(newNodeTwo.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.awaitSecondaryNodes();

jsTestLog("Checking that the 'newlyAdded' field is no longer set on either node");
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1);
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 2);

jsTestLog("Checking vote counts after secondaries have finished initial sync");
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
