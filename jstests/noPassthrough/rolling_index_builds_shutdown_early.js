/**
 * Tests that secondaries already containing an index build started by the primary node vote for
 * committing the index immediately and do not raise an exception if a shutdown occurs before the
 * verdict.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const replTest = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            }
        },
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            }
        },
    ]
});

replTest.startSet();
replTest.initiate();

const dbName = 'test';
const collName = 't';

TestData.dbName = dbName;
TestData.collName = collName;

let primary = replTest.getPrimary();
let primaryDB = primary.getDB(dbName);
let coll = primaryDB.getCollection(collName);

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary) ||
    !IndexBuildTest.indexBuildCommitQuorumEnabled(primary)) {
    jsTestLog('Two phase index builds or commit quorum are not supported, skipping test.');
    replTest.stopSet();
    return;
}

// Populate the collection to avoid empty collection index build optimization.
assert.commandWorked(coll.insert({x: 1, y: 1}));

// Make sure the documents make it to the secondaries.
replTest.awaitReplication();

let secondaries = replTest.getSecondaries();
const standalonePort = allocatePort();
jsTestLog('Standalone server will listen on port: ' + standalonePort);

function buildIndexOnNodeAsStandalone(node) {
    jsTestLog('A. Restarting as standalone: ' + node.host);
    replTest.stop(node, /*signal=*/null, /*opts=*/null, {forRestart: true, waitpid: true});
    const standalone = MongoRunner.runMongod({
        restart: true,
        dbpath: node.dbpath,
        port: standalonePort,
        setParameter: {
            disableLogicalSessionCacheRefresh: true,
            ttlMonitorEnabled: false,
        },
    });
    if (jsTestOptions().keyFile) {
        assert(jsTest.authenticate(standalone),
               'Failed authentication during restart: ' + standalone.host);
    }

    jsTestLog('B. Building index on standalone: ' + standalone.host);
    const standaloneDB = standalone.getDB(dbName);
    const standaloneColl = standaloneDB.getCollection(collName);
    assert.commandWorked(standaloneColl.createIndex({x: 1}, {name: 'rolling_index_x_1'}));

    jsTestLog('C. Restarting as replica set node: ' + node.host);
    MongoRunner.stopMongod(standalone);
    replTest.restart(node);
    replTest.awaitReplication();

    let mongo = new Mongo(node.host);
    mongo.setSlaveOk(true);
    let indexes = mongo.getDB(dbName).getCollection(collName).getIndexes();
    assert.eq(2, indexes.length);
}

buildIndexOnNodeAsStandalone(secondaries[0]);

jsTestLog('D. Repeat the procedure for the remaining secondary: ' + secondaries[1].host);
buildIndexOnNodeAsStandalone(secondaries[1]);

replTest.awaitNodesAgreeOnPrimary(
    replTest.kDefaultTimeoutMS, replTest.nodes, replTest.getNodeId(primary));

secondaries = replTest.getSecondaries();

// Enable fail points on the secondaries to prevent them from sending the commit quorum vote to the
// primary.
assert.commandWorked(secondaries[0].adminCommand(
    {configureFailPoint: 'hangBeforeSendingCommitQuorumVote', mode: 'alwaysOn'}));
assert.commandWorked(secondaries[1].adminCommand(
    {configureFailPoint: 'hangBeforeSendingCommitQuorumVote', mode: 'alwaysOn'}));

jsTestLog('E. Build index on the primary: ' + primary.host);
startParallelShell(() => {
    const coll = db.getSiblingDB(TestData.dbName).getCollection(TestData.collName);
    coll.createIndex({x: 1}, {name: 'rolling_index_x_1'}, 3);
}, primary.port);

checkLog.containsJson(secondaries[0], 4709501);
checkLog.containsJson(secondaries[1], 4709501);

replTest.stop(secondaries[0], /*signal=*/null, /*opts=*/null, {waitpid: true});
replTest.stop(secondaries[1], /*signal=*/null, /*opts=*/null, {waitpid: true});

replTest.stop(primary, /*signal=*/null, /*opts=*/null, {waitpid: true});
}());
