/**
 * This test simulates initial sync workflows which are performed by the Atlas automation agent.
 */
(function() {
"use strict";

load('jstests/replsets/rslib.js');  // waitForState.

const testName = TestData.testName;
// Set up a standard 3-node replica set.  Note the two secondaries are priority 0; this is
// different than the real Atlas configuration where the secondaries would be electable.
const rst = new ReplSetTest({
    name: testName,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    useBridge: true,
    // We shorten the election timeout period so the tests with an unhealthy set run and recover
    // faster.
    settings: {electionTimeoutMillis: 2000, heartbeatIntervalMillis: 400}
});
rst.startSet();
rst.initiate();
// Add some data.
const primary = rst.getPrimary();
const testDb = primary.getDB("test");
assert.commandWorked(testDb[testName].insert([{a: 1}, {b: 2}, {c: 3}]));
rst.awaitReplication();

function disconnectSecondaries(secondariesDown) {
    for (let i = 1; i <= secondariesDown; i++) {
        for (const node of rst.nodes) {
            if (node !== rst.nodes[i]) {
                node.disconnect(rst.nodes[i]);
            }
        }
    }
}

function reconnectSecondaries() {
    for (const node of rst.nodes) {
        for (const node2 of rst.nodes) {
            if (node2 !== node) {
                node2.reconnect(node);
            }
        }
    }
}

function testAddWithInitialSync(secondariesDown) {
    let config = rst.getReplSetConfigFromNode();
    secondariesDown = secondariesDown || 0;
    disconnectSecondaries(secondariesDown);
    const useForce = secondariesDown > 1;
    if (useForce) {
        // Wait for the set to become unhealthy.
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    }
    // Atlas always adds nodes with 0 votes and priority
    const newNode = rst.add({rsConfig: {votes: 0, priority: 0}});
    // The second disconnect ensures we can't reach the new node from the 'down' nodes.
    disconnectSecondaries(secondariesDown);
    const newConfig = rst.getReplSetConfig();
    config.members = newConfig.members;
    config.version += 1;
    jsTestLog("Reconfiguring set to add node.");
    assert.commandWorked(primary.adminCommand(
        {replSetReconfig: config, maxTimeMS: ReplSetTest.kDefaultTimeoutMS, force: useForce}));

    jsTestLog("Waiting for node to sync.");
    rst.waitForState(newNode, ReplSetTest.State.SECONDARY);

    jsTestLog("Reconfiguring added node to have votes");
    config = rst.getReplSetConfigFromNode(primary.nodeId);
    config.version += 1;
    config.members[3].votes = 1;
    assert.commandWorked(primary.adminCommand(
        {replSetReconfig: config, maxTimeMS: ReplSetTest.kDefaultTimeoutMS, force: useForce}));
    if (!useForce) {
        // Make sure we can replicate to it.  This only works if the set was healthy, otherwise we
        // can't.
        assert.commandWorked(
            testDb[testName].insert({addWithInitialSync: secondariesDown}, {writeConcern: {w: 1}}));
        rst.awaitReplication(undefined, undefined, [newNode]);
    }

    // Make sure the set is still consistent after adding the node.
    reconnectSecondaries();
    // If we were in a majority-down scenario, wait for the primary to be re-elected.
    assert.soon(() => primary == rst.getPrimary());
    rst.checkOplogs();
    rst.checkReplicatedDataHashes();

    // Remove our extra node.
    rst.stop(newNode);
    rst.remove(newNode);
    rst.reInitiate();
}

function testReplaceWithInitialSync(secondariesDown) {
    const nodeToBeReplaced = rst.getSecondaries()[2];
    secondariesDown = secondariesDown || 0;
    const useForce = secondariesDown > 1;
    let config = rst.getReplSetConfigFromNode(primary.nodeId);
    disconnectSecondaries(secondariesDown);
    if (useForce) {
        // Wait for the set to become unhealthy.
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    }

    let nodeId = rst.getNodeId(nodeToBeReplaced);
    const nodeVotes = config.members[nodeId].votes;
    const highestMemberId = config.members[nodeId]._id;
    if (nodeVotes > 0) {
        jsTestLog("Reconfiguring to remove the node.");
        config.version += 1;
        config.members.splice(nodeId, 1);
        assert.commandWorked(primary.adminCommand(
            {replSetReconfig: config, maxTimeMS: ReplSetTest.kDefaultTimeoutMS, force: useForce}));
    }

    jsTestLog("Stopping node for replacement");
    rst.stop(nodeToBeReplaced, undefined, {skipValidation: true}, {forRestart: true});
    rst.remove(nodeToBeReplaced);
    if (!useForce) {
        // Add some data.  This can't work in a majority-down situation, so we don't do it then.
        assert.commandWorked(testDb[testName].insert({replaceWithInitialSync: secondariesDown},
                                                     {writeConcern: {w: 1}}));
    }

    jsTestLog("Starting a new replacement node with empty data directory.");
    const replacementNode = rst.add({rsConfig: {votes: 0, priority: 0}});
    // The second disconnect ensures we can't reach the replacement node from the 'down' nodes.
    disconnectSecondaries(secondariesDown);

    nodeId = rst.getNodeId(replacementNode);
    config = rst.getReplSetConfigFromNode(primary.nodeId);
    const newConfig = rst.getReplSetConfig();
    config.members = newConfig.members;
    // Don't recycle the member id.
    config.members[nodeId]._id = highestMemberId + 1;
    config.version += 1;
    jsTestLog("Reconfiguring set to add the replacement node.");
    assert.commandWorked(primary.adminCommand(
        {replSetReconfig: config, maxTimeMS: ReplSetTest.kDefaultTimeoutMS, force: useForce}));

    jsTestLog("Waiting for the replacement node to sync.");
    rst.waitForState(replacementNode, ReplSetTest.State.SECONDARY);

    if (nodeVotes > 0) {
        jsTestLog("Reconfiguring the replacement node to have votes.");
        config = rst.getReplSetConfigFromNode(primary.nodeId);
        config.version += 1;
        config.members[nodeId].votes = nodeVotes;
        assert.commandWorked(primary.adminCommand(
            {replSetReconfig: config, maxTimeMS: ReplSetTest.kDefaultTimeoutMS, force: useForce}));
    }
    if (!useForce) {
        // Make sure we can replicate to it, if the set is otherwise healthy.
        rst.awaitReplication(undefined, undefined, [replacementNode]);
    }
    // Make sure the set is still consistent after resyncing the node.
    reconnectSecondaries();
    // If we were in a majority-down scenario, wait for the primary to be re-elected.
    assert.soon(() => primary == rst.getPrimary());
    rst.checkOplogs();
    rst.checkReplicatedDataHashes();
}

jsTestLog("Test adding a node with initial sync in a healthy system.");
testAddWithInitialSync(0);

jsTestLog("Test adding a node with initial sync with one secondary unreachable.");
testAddWithInitialSync(1);

jsTestLog("Test adding a node with initial sync with two secondaries unreachable.");
testAddWithInitialSync(2);

jsTestLog("Adding node for replace-node scenarios");
const newNode = rst.add({rsConfig: {priority: 0}});
rst.reInitiate();
// Wait for the node to to become secondary.
waitForState(newNode, ReplSetTest.State.SECONDARY);

jsTestLog("Test replacing a node with initial sync in a healthy system.");
testReplaceWithInitialSync(0);

jsTestLog("Test replacing a node with initial sync with one secondary unreachable.");
testReplaceWithInitialSync(1);

jsTestLog("Test replacing a node with initial sync with two secondaries unreachable.");
testReplaceWithInitialSync(2);

rst.stopSet();
})();
