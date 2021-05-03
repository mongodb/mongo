/**
 * Tests the correct workflow for adding a voting electable node in a PSA set and ensures that no
 * committed writes will be rolled back after that workflow. We start with a PSA set. We shut down
 * the secondary and reconfigure it to have votes 0. Then, we do a majority write that will commit
 * only on the primary, so that the secondary is now missing a majority committed write. Next, we
 * test the correct workflow, which involves two reconfigs:
 *
 * 1) Give the secondary votes: 1 but priority: 0. This will not allow the stale secondary to run
 * for election
 *
 * 2) Increase the priority on the secondary. With this reconfig, because of the Oplog
 * Committment rule, the secondary must have the previously committed write, and so it can safely
 * become the primary
 *
 * Finally, we step up the secondary and verify that the oplog entry was not rolled back.
 *
 * @tags: [requires_fcv_50]
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");
load("jstests/libs/write_concern_util.js");

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [{}, {}, {rsConfig: {arbiterOnly: true}}],
});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const collName = jsTestName();
const primary = rst.getPrimary();
assert.eq(primary, rst.nodes[0], "the primary should be the node at index 0");

const testDb = primary.getDB("test");
assert.commandWorked(testDb[collName].insert({a: 1}, {writeConcern: {w: "majority"}}));

assertVoteCount(primary, {
    votingMembersCount: 3,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 3,
});

jsTestLog("Stop the secondary, which should be node 1");
rst.stop(1);

jsTestLog("Do a majority write that fails waiting for write concern");
let res = testDb.runCommand(
    {insert: collName, documents: [{a: 2}], writeConcern: {w: "majority", wtimeout: 3 * 1000}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
checkWriteConcernTimedOut(res);

// In config C0, the secondary will have 'votes: 0' and 'priority: 0'.
let config = rst.getReplSetConfigFromNode();
jsTestLog("Original config: " + tojson(config));
config.members[1].votes = 0;
config.members[1].priority = 0;
config.version += 1;
jsTestLog("Reconfiguring set to remove the secondary's vote. Config C0: " + tojson(config));
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

assertVoteCount(primary, {
    votingMembersCount: 2,
    majorityVoteCount: 2,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 3,
});

jsTestLog("Do a majority write that succeeds");
// The secondary will not have this write because it was shut down.
assert.commandWorked(testDb[collName].insert({a: 3}, {writeConcern: {w: "majority"}}));

// At this point, the majority vote count is greater than the writable voting members count, since
// the secondary still has 'votes: 0'. This indicates that there may not be an overlap between the
// election quorum and the write quorum.

// As a result, if we make the secondary a voter AND electable in a future reconfig, it is possible
// for the secondary to be elected without the recent majority committed write. To avoid this, when
// making the secondary a voting node again, first configure the secondary to have 'priority: 0', so
// that it is not electable. Label this config 'C1'.
config = rst.getReplSetConfigFromNode(primary.nodeId);
config.members[1].votes = 1;
config.members[1].priority = 0;
config.version += 1;
jsTestLog(
    "Reconfiguring set to re-enable the secondary's vote and make it unelectable. Config C1: " +
    tojson(config));
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

// The next reconfig, C2, will increase the priority of the secondary, so that it can
// run for election. This is safe due to the Oplog Committment rule, which guarantees that anything
// committed in C0 will also be committed in C1.
config = rst.getReplSetConfigFromNode(primary.nodeId);
config.members[1].priority = 1;
config.version += 1;
jsTestLog("Reconfiguring set to allow the secondary to run for election. Config C2: " +
          tojson(config));

// Since the secondary is currently down, this reconfig will hang on waiting for the previous
// majority write to be committed in the current config, C1.
assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config, maxTimeMS: 3 * 1000}),
                             ErrorCodes.CurrentConfigNotCommittedYet);

// After restarting the secondary, this reconfig should succeed.
jsTestLog("Restarting the secondary");
rst.restart(1);
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

assertVoteCount(primary, {
    votingMembersCount: 3,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 3,
});

jsTestLog("Stepping up the secondary");
assert.soonNoExcept(() => {
    assert.commandWorked(rst.nodes[1].adminCommand({replSetStepUp: 1}));
    assert.eq(rst.getPrimary(), rst.nodes[1]);
    return true;
});

// Verify that the committed write was not rolled back.
assert.eq(rst.nodes[0].getDB("test")[collName].find({a: 3}).itcount(), 1);
assert.eq(rst.nodes[1].getDB("test")[collName].find({a: 3}).itcount(), 1);

rst.stopSet();
})();
