/**
 * Builds an index on a subset of nodes in a rolling fashion. Tests that building the same index
 * with a primary that doesn't have the index logs a message on the secondaries that the index build
 * commit quorum may not be achieved.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const replTest = new ReplSetTest({nodes: 3});
const nodes = replTest.startSet();
replTest.initiate();

const dbName = 'test';
const collName = 't';

let primary = replTest.getPrimary();
let primaryDB = primary.getDB(dbName);
let primaryColl = primaryDB.getCollection(collName);

// Populate collection to avoid empty collection optimization.
const numDocs = 100;
for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(primaryColl.insert({x: i}));
}

// Make sure the documents make it to the secondaries.
replTest.awaitReplication();

const secondaries = replTest.getSecondaries();
assert.eq(nodes.length - 1,
          secondaries.length,
          'unexpected number of secondaries: ' + tojson(secondaries));

const standalonePort = allocatePort();
jsTestLog('Standalone server will listen on port: ' + standalonePort);

// Build the index on the secondaries only.
IndexBuildTest.buildIndexOnNodeAsStandalone(
    replTest, secondaries[0], standalonePort, dbName, collName, {x: 1}, 'x_1');
IndexBuildTest.buildIndexOnNodeAsStandalone(
    replTest, secondaries[1], standalonePort, dbName, collName, {x: 1}, 'x_1');

replTest.awaitNodesAgreeOnPrimary(
    replTest.kDefaultTimeoutMS, replTest.nodes, replTest.getNodeId(primary));

// TODO(SERVER-71768): fix the index build stall.
jsTestLog('Build index on the primary as part of the replica set: ' + primary.host);
let createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {x: 1}, {name: 'x_1'}, [ErrorCodes.Interrupted]);

// When the index build starts, find its op id. This will be the op id of the client connection, not
// the thread pool task managed by IndexBuildsCoordinatorMongod.
const filter = {
    "desc": {$regex: /conn.*/}
};
let opId = IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'x_1', filter);

checkLog.containsJson(secondaries[0], 7176900);
checkLog.containsJson(secondaries[1], 7176900);
clearRawMongoProgramOutput();

assert.commandWorked(primaryDB.killOp(opId));
createIdx();

// Test building multiple indexes, some of which exist on the secondary.
createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), [{x: 1}, {y: 1}], {}, [ErrorCodes.Interrupted]);

checkLog.containsJson(secondaries[0], 7176900);
checkLog.containsJson(secondaries[1], 7176900);

opId = IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'x_1', filter);
assert.commandWorked(primaryDB.killOp(opId));

createIdx();

// TODO(SERVER-71768): Check dbHash.
TestData.skipCheckDBHashes = true;
replTest.stopSet();
}());
