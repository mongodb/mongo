/**
 * Builds an index in a 3-node replica set in a rolling fashion. This test implements the procedure
 * documented in:
 *     https://docs.mongodb.com/manual/tutorial/build-indexes-on-replica-sets/
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

// Set up replica set
const replTest = new ReplSetTest({nodes: 3});

const nodes = replTest.startSet();
replTest.initiate();

const dbName = 'test';
const collName = 't';

// Populate collection to avoid empty collection optimization.
function insertDocs(coll, startId, numDocs) {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        const v = startId + i;
        bulk.insert({_id: v, a: v, b: v});
    }
    assert.commandWorked(bulk.execute());
}

let primary = replTest.getPrimary();
let primaryDB = primary.getDB(dbName);
let coll = primaryDB.getCollection(collName);

const numDocs = 100;
insertDocs(coll, 0, numDocs);
assert.eq(numDocs, coll.count(), 'unexpected number of documents after bulk insert.');

// Make sure the documents make it to the secondaries.
replTest.awaitReplication();

// Ensure we can create an index through replication.
assert.commandWorked(coll.createIndex({a: 1}, {name: 'replicated_index_a_1'}));

const secondaries = replTest.getSecondaries();
assert.eq(nodes.length - 1,
          secondaries.length,
          'unexpected number of secondaries: ' + tojson(secondaries));

const standalonePort = allocatePort();
jsTestLog('Standalone server will listen on port: ' + standalonePort);

IndexBuildTest.buildIndexOnNodeAsStandalone(
    replTest, secondaries[0], standalonePort, dbName, collName, {b: 1}, 'rolling_index_b_1');

jsTestLog('Repeat the procedure for the remaining secondary: ' + secondaries[1].host);
IndexBuildTest.buildIndexOnNodeAsStandalone(
    replTest, secondaries[1], standalonePort, dbName, collName, {b: 1}, 'rolling_index_b_1');

replTest.awaitNodesAgreeOnPrimary(
    replTest.kDefaultTimeoutMS, replTest.nodes, replTest.getNodeId(primary));

jsTestLog('Build index on the primary: ' + primary.host);
assert.commandWorked(primaryDB.adminCommand({replSetStepDown: 60}));
const newPrimary = replTest.getPrimary();
jsTestLog('Stepped down primary for index build: ' + primary.host +
          '. New primary elected: ' + newPrimary.host);
IndexBuildTest.buildIndexOnNodeAsStandalone(
    replTest, primary, standalonePort, dbName, collName, {b: 1}, 'rolling_index_b_1');

// Ensure we can create an index after doing a rolling index build.
let newPrimaryDB = newPrimary.getDB(dbName);
let newColl = newPrimaryDB.getCollection(collName);
assert.commandWorked(newColl.createIndex({a: 1, b: 1}, {name: 'post_rolling_index_a_1_b_1'}));

insertDocs(newColl, numDocs, numDocs);
assert.eq(numDocs * 2, newColl.count(), 'unexpected number of documents after bulk insert.');

replTest.stopSet();
}());
