/**
 * Verify that force reconfigs overwrite the 'newlyAdded' field correctly in a replica set.
 *
 * @tags: [ requires_fcv_46 ]
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");
load('jstests/libs/fail_point_util.js');

const rst = new ReplSetTest(
    {name: jsTestName(), nodes: 1, nodeOptions: {setParameter: {enableAutomaticReconfig: true}}});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();

const dbName = "testdb";
const collName = "testcoll";
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

// TODO (SERVER-46808): Move this into ReplSetTest.initiate
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 0);
assert.eq(false, isMemberNewlyAdded(primary, 0));

assert.commandWorked(primaryColl.insert({"starting": "doc"}));

assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 1,
});

const addNode = (id, {newlyAdded, force, shouldSucceed, failureCode} = {}) => {
    jsTestLog(`Adding node ${id}, newlyAdded: ${newlyAdded}, force: ${force}, shouldSucceed: ${
        shouldSucceed}, failureCode: ${failureCode}`);

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

    let newNodeObj = {
        _id: id,
        host: newNode.host,
        priority: 0,
    };

    if (newlyAdded) {
        newNodeObj.newlyAdded = true;
    }

    let config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
    config.version++;
    config.members.push(newNodeObj);

    if (!shouldSucceed) {
        jsTestLog("Running reconfig with bad config " + tojsononeline(config));

        assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config, force: force}),
                                     failureCode);
        rst.remove(newNode);
        return null;
    }

    jsTestLog("Running reconfig with valid config " + tojsononeline(config));
    assert(!failureCode);
    assert.commandWorked(primary.adminCommand({replSetReconfig: config, force: force}));
    waitForConfigReplication(primary, rst.nodes);

    assert.commandWorked(newNode.adminCommand({
        waitForFailPoint: "initialSyncHangBeforeFinish",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }));

    return newNode;
};

jsTestLog("Fail adding a new node with 'newlyAdded' with force reconfig");
addNode(2, {
    newlyAdded: true,
    force: true,
    shouldSucceed: false,
    failureCode: ErrorCodes.InvalidReplicaSetConfig
});
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 1,
});

jsTestLog("Fail adding a new node with 'newlyAdded' with safe reconfig");
addNode(2, {
    newlyAdded: true,
    force: false,
    shouldSucceed: false,
    failureCode: ErrorCodes.InvalidReplicaSetConfig
});
assertVoteCount(primary, {
    votingMembersCount: 1,
    majorityVoteCount: 1,
    writableVotingMembersCount: 1,
    writeMajorityCount: 1,
    totalMembersCount: 1,
});

jsTestLog("Add a new node without 'newlyAdded' with force reconfig");
const firstNewNode = addNode(2, {newlyAdded: false, force: true, shouldSucceed: true});
assert.eq(false, isMemberNewlyAdded(primary, 1, true /* force */));
assertVoteCount(primary, {
    votingMembersCount: 2,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 2,
});

jsTestLog("Add a new node without 'newlyAdded' with safe reconfig");
const secondNewNode = addNode(3, {newlyAdded: false, force: false, shouldSucceed: true});
assert.eq(false, isMemberNewlyAdded(primary, 1, false /* force */));
assert.eq(true, isMemberNewlyAdded(primary, 2, false /* force */));
assertVoteCount(primary, {
    votingMembersCount: 2,
    majorityVoteCount: 2,
    writableVotingMembersCount: 2,
    writeMajorityCount: 2,
    totalMembersCount: 3,
});

jsTestLog(
    "Add a new node without 'newlyAdded' with force reconfig, squashing old 'newlyAdded' fields");
const thirdNewNode = addNode(4, {newlyAdded: false, force: true, shouldSucceed: true});
assert.eq(false, isMemberNewlyAdded(primary, 1, true /* force */));
assert.eq(false, isMemberNewlyAdded(primary, 2, true /* force */));
assert.eq(false, isMemberNewlyAdded(primary, 3, true /* force */));
assertVoteCount(primary, {
    votingMembersCount: 4,
    majorityVoteCount: 3,
    writableVotingMembersCount: 4,
    writeMajorityCount: 3,
    totalMembersCount: 4,
});

assert.commandWorked(
    firstNewNode.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
assert.commandWorked(
    secondNewNode.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
assert.commandWorked(
    thirdNewNode.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));

rst.waitForState(firstNewNode, ReplSetTest.State.SECONDARY);
rst.waitForState(secondNewNode, ReplSetTest.State.SECONDARY);
rst.waitForState(thirdNewNode, ReplSetTest.State.SECONDARY);

jsTestLog("Making sure the set can accept writes with write concerns");
assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 4}}));
assert.commandWorked(
    primaryColl.insert({"steady": "state_majority"}, {writeConcern: {w: 'majority'}}));

assertVoteCount(primary, {
    votingMembersCount: 4,
    majorityVoteCount: 3,
    writableVotingMembersCount: 4,
    writeMajorityCount: 3,
    totalMembersCount: 4,
});

rst.stopSet();
})();
