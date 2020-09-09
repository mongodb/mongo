/**
 * This test simulates initial sync workflows which are performed by the Atlas automation agent.
 * @tags: [live_record_incompatible]
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

function testAddWithInitialSync(secondariesDown) {
    let config = rst.getReplSetConfigFromNode();
    secondariesDown = secondariesDown || 0;
    disconnectSecondaries(rst, secondariesDown);
    const majorityDown = secondariesDown > 1;
    if (majorityDown) {
        // Wait for the set to become unhealthy.
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    }
    // Atlas always adds nodes with 0 votes and priority
    const newNode = rst.add({rsConfig: {votes: 0, priority: 0}});
    // The second disconnect ensures we can't reach the new node from the 'down' nodes.
    disconnectSecondaries(rst, secondariesDown);
    const newConfig = rst.getReplSetConfig();
    config.members = newConfig.members;
    config.version += 1;
    jsTestLog("Reconfiguring set to add node.");
    assert.commandWorked(primary.adminCommand(
        {replSetReconfig: config, maxTimeMS: ReplSetTest.kDefaultTimeoutMS, force: majorityDown}));

    jsTestLog("Waiting for node to sync.");
    rst.waitForState(newNode, ReplSetTest.State.SECONDARY);

    jsTestLog("Reconfiguring added node to have votes");
    config = rst.getReplSetConfigFromNode(primary.nodeId);
    config.version += 1;
    config.members[3].votes = 1;
    assert.commandWorked(primary.adminCommand(
        {replSetReconfig: config, maxTimeMS: ReplSetTest.kDefaultTimeoutMS, force: majorityDown}));
    if (!majorityDown) {
        // Make sure we can replicate to it.  This only works if the set was healthy, otherwise we
        // can't.
        assert.commandWorked(
            testDb[testName].insert({addWithInitialSync: secondariesDown}, {writeConcern: {w: 1}}));
        rst.awaitReplication(undefined, undefined, [newNode]);
    }

    // Make sure the set is still consistent after adding the node.
    reconnectSecondaries(rst);
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
    // Wait for existing reconfigs to complete.
    rst.waitForAllNewlyAddedRemovals();

    const node = rst.getSecondaries()[2];
    const majorityDown = secondariesDown > 1;
    disconnectSecondaries(rst, secondariesDown);
    if (majorityDown) {
        // Wait for the set to become unhealthy.
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    }

    jsTestLog("Stopping node for replacement of data");
    rst.stop(node, undefined, {skipValidation: true}, {forRestart: true});
    if (!majorityDown) {
        // Add some data.  This can't work in a majority-down situation, so we don't do it then.
        assert.commandWorked(testDb[testName].insert({replaceWithInitialSync: secondariesDown},
                                                     {writeConcern: {w: 1}}));
    }

    jsTestLog("Starting a new replacement node with empty data directory.");
    rst.start(node, {startClean: true}, true /* restart */);
    // We can't use awaitSecondaryNodes because the set might not be healthy.
    assert.soonNoExcept(() => node.adminCommand({isMaster: 1}).secondary);

    if (!majorityDown) {
        // Make sure we can replicate to it, if the set is otherwise healthy.
        rst.awaitReplication(undefined, undefined, [node]);
    }
    // Make sure the set is still consistent after resyncing the node.
    reconnectSecondaries(rst);
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
