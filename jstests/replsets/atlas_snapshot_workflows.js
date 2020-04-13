/**
 * This test simulates workflows which are performed by the Atlas automation agent, where nodes are
 * created or restarted using file system snapshots.
 *
 * @tags: [requires_persistence,requires_wiredtiger]
 */

// Set up a standard 3-node replica set.  Note the two secondaries are priority 0; this is
// different than the real Atlas configuration where the secondaries would be electable.
(function() {
"use strict";

// Snapshot works only on enterprise.
if (!buildInfo()["modules"].includes("enterprise")) {
    printjson(buildInfo()["modules"]);
    jsTestLog("Skipping snapshot tests because not running on enterprise.");
    return 0;
}

load("jstests/libs/backup_utils.js");

const testName = TestData.testName;
const rst = new ReplSetTest({
    name: testName,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    useBridge: true,
    // We shorten the election timeout period so the tests with an unhealthy set run and recover
    // faster.
    settings: {electionTimeoutMillis: 2000}
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

function testAddWithSnapshot(secondariesDown) {
    const newdbpath = MongoRunner.dataPath + "newNode";
    jsTestLog("Making snapshot of primary");
    backupData(primary, newdbpath);
    // Add some data after the backup.
    assert.commandWorked(testDb[testName].insert({addWithSnapshotAfterSnapshot: secondariesDown},
                                                 {writeConcern: {w: 1}}));
    let config = rst.getReplSetConfigFromNode();
    secondariesDown = secondariesDown || 0;
    disconnectSecondaries(secondariesDown);
    const useForce = secondariesDown > 1;
    if (useForce) {
        // Wait for the set to become unhealthy.
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    }
    // Atlas always adds nodes with 0 votes and priority
    const newNode =
        rst.add({rsConfig: {votes: 0, priority: 0}, noCleanData: true, dbpath: newdbpath});
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
            testDb[testName].insert({addWithSnapshot: secondariesDown}, {writeConcern: {w: 1}}));
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
    resetDbpath(newdbpath);
}

function testReplaceWithSnapshot(node, secondariesDown) {
    secondariesDown = secondariesDown || 0;
    const useForce = secondariesDown > 1;
    const replacedbpath = rst.getDbPath(node);
    const backupdbpath = replacedbpath + "_bak";
    let config = rst.getReplSetConfigFromNode();
    jsTestLog("Backing up the primary node");
    backupData(primary, backupdbpath);
    // Add some data after the backup.
    assert.commandWorked(
        testDb[testName].insert({replaceWithSnapshot: secondariesDown}, {writeConcern: {w: 1}}));
    disconnectSecondaries(secondariesDown);
    if (useForce) {
        // Wait for the set to become unhealthy.
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
    }
    jsTestLog("Stopping node for replacement of data");
    rst.stop(node, undefined, undefined, {forRestart: true});

    jsTestLog("Replacing node data with snapshot");
    copyDbpath(backupdbpath, replacedbpath);
    resetDbpath(backupdbpath);
    jsTestLog("Restarting replacement node.");
    rst.start(node, undefined, true /* restart */);
    // We can't use awaitSecondaryNodes because the set might not be healthy.
    assert.soonNoExcept(() => node.adminCommand({isMaster: 1}).secondary);
    if (!useForce) {
        // Make sure we can replicate to it, if the set is otherwise healthy.
        rst.awaitReplication(undefined, undefined, [node]);
    }
    // Make sure the set is still consistent after resyncing the node.
    reconnectSecondaries();
    // If we were in a majority-down scenario, wait for the primary to be re-elected.
    assert.soon(() => primary == rst.getPrimary());
    rst.checkOplogs();
    rst.checkReplicatedDataHashes();
}

jsTestLog("Test adding a node with snapshot in a healthy system.");
testAddWithSnapshot(0);

jsTestLog("Test adding a node with snapshot with one secondary unreachable.");
testAddWithSnapshot(1);

jsTestLog("Test adding a node with snapshot with two secondaries unreachable.");
testAddWithSnapshot(2);

jsTestLog("Adding node for replace-node scenarios");
let newNode = rst.add({rsConfig: {priority: 0}});
rst.reInitiate();

jsTestLog("Test replacing a node with snapshot in a healthy system.");
testReplaceWithSnapshot(newNode, 0);

jsTestLog("Test replacing a node with snapshot with one secondary unreachable.");
testReplaceWithSnapshot(newNode, 1);

jsTestLog("Test replacing a node with snapshot with two secondaries unreachable.");
testReplaceWithSnapshot(newNode, 2);

rst.stopSet();
})();
