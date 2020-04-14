/*
 * Verify that force reconfigs set the 'newlyAdded' field correctly in a replica set. We test this
 * by starting with a two node replica set. We first do a force reconfig that adds a node with
 * 'newlyAdded: true' and verify that it correctly sets the 'newlyAdded' field for that member. We
 * then do another force reconfig that adds a second node without the 'newlyAdded' field passed in
 * and verify that the node does not have 'newlyAdded' appended to it.
 *
 * TODO(SERVER-46592): This test is multiversion-incompatible in 4.6.  If we use 'requires_fcv_46'
 *                     as the tag for that, removing 'requires_fcv_44' is sufficient.  Otherwise,
 *                     please set the appropriate tag when removing 'requires_fcv_44'
 * @tags: [requires_fcv_44, requires_fcv_46]
 *
 * TODO (SERVER-47331): Determine if this test needs to be altered, since this test will do a force
 *                      reconfig followed by a safe reconfig.
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {setParameter: {enableAutomaticReconfig: true}}
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "testdb";
const collName = "testcoll";
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

// TODO (SERVER-46808): Move this into ReplSetTest.initiate
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 0);
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1);
waitForConfigReplication(primary, rst.nodes);

// Verify that the 'newlyAdded' fields of the first two nodes in the repl set have been removed.
assert.eq(false, isMemberNewlyAdded(primary, 0));
assert.eq(false, isMemberNewlyAdded(primary, 1));

assert.commandWorked(primaryColl.insert({"starting": "doc"}, {writeConcern: {w: 2}}));

// Verify that the counts and majorities were set correctly after spinning up the repl set.
let status = assert.commandWorked(primaryDb.adminCommand({replSetGetStatus: 1}));
assert.eq(status["votingMembersCount"], 2, status);
assert.eq(status["majorityVoteCount"], 2, status);
assert.eq(status["writableVotingMembersCount"], 2, status);
assert.eq(status["writeMajorityCount"], 2, status);

const addNodeThroughForceReconfig = (id, setNewlyAdded) => {
    const newNode = rst.add({
        rsConfig: {priority: 0},
        setParameter: {
            'failpoint.forceSyncSourceCandidate':
                tojson({mode: 'alwaysOn', data: {"hostAndPort": primary.host}}),
            'failpoint.initialSyncHangBeforeFinish': tojson({mode: 'alwaysOn'}),
            'numInitialSyncAttempts': 1,
            'enableAutomaticReconfig': true
        }
    });

    const newNodeObj = {
        _id: id,
        host: newNode.host,
        priority: 0,
    };

    // Do not set the 'newlyAdded' field if 'setNewlyAdded' is false.
    if (setNewlyAdded) {
        newNodeObj.newlyAdded = true;
    }

    // Get the config from disk in case there are any 'newlyAdded' fields we should preserve.
    let config = primary.getDB("local").system.replset.findOne();
    config.version++;
    config.members.push(newNodeObj);

    assert.commandWorked(primary.adminCommand({replSetReconfig: config, force: true}));
    waitForConfigReplication(primary, rst.nodes);

    return newNode;
};

jsTestLog("Adding the first new node to config (with 'newlyAdded' set)");
const firstNewNodeId = 2;
const firstNewNode = addNodeThroughForceReconfig(firstNewNodeId, true /* newlyAdded */);

assert.commandWorked(firstNewNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout,
}));

jsTestLog("Checking for 'newlyAdded' field on the first new node");
assert.eq(true, isMemberNewlyAdded(primary, firstNewNodeId, true /* force */));

// Verify that the counts and majorities were not changed after adding the first new node, since the
// node should not be treated as a voting node.
status = assert.commandWorked(primaryDb.adminCommand({replSetGetStatus: 1}));
assert.eq(status["votingMembersCount"], 2, status);
assert.eq(status["majorityVoteCount"], 2, status);
assert.eq(status["writableVotingMembersCount"], 2, status);
assert.eq(status["writeMajorityCount"], 2, status);

jsTestLog("Adding the second new node to config (with 'newlyAdded' not set)");
const secondNewNodeId = 3;
const secondNewNode = addNodeThroughForceReconfig(secondNewNodeId, false /* newlyAdded */);

assert.commandWorked(secondNewNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout,
}));

jsTestLog("Verifying that 'newlyAdded' field is still set on the first node");
assert.eq(true, isMemberNewlyAdded(primary, firstNewNodeId, true /* force */));

jsTestLog("Verifying that 'newlyAdded' field is not set on the second node");
assert.eq(false, isMemberNewlyAdded(primary, secondNewNodeId, true /* force */));

// Verify that the counts and majorities were updated correctly after the second node was added to
// the repl set.
status = assert.commandWorked(primaryDb.adminCommand({replSetGetStatus: 1}));
assert.eq(status["votingMembersCount"], 3, status);
assert.eq(status["majorityVoteCount"], 2, status);
assert.eq(status["writableVotingMembersCount"], 3, status);
assert.eq(status["writeMajorityCount"], 2, status);

assert.commandWorked(
    firstNewNode.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
assert.commandWorked(
    secondNewNode.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));

rst.waitForState(firstNewNode, ReplSetTest.State.SECONDARY);
rst.waitForState(secondNewNode, ReplSetTest.State.SECONDARY);

waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 2, true /* force */);

jsTestLog("Making sure the set can accept w:4 writes");
assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 4}}));

// Verify that the first new node had its 'newlyAdded' field removed.
assert.eq(false, isMemberNewlyAdded(primary, firstNewNodeId, true /* force */));

// Verify that the counts and majorities were updated correctly after the 'newlyAdded' field was
// removed.
status = assert.commandWorked(primaryDb.adminCommand({replSetGetStatus: 1}));
assert.eq(status["votingMembersCount"], 4, status);
assert.eq(status["majorityVoteCount"], 3, status);
assert.eq(status["writableVotingMembersCount"], 4, status);
assert.eq(status["writeMajorityCount"], 3, status);

rst.stopSet();
})();
