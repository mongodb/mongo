/**
 * Tests that a new replica set member performing an initial sync from a secondary node as the sync
 * source, which has an in-progress index build will also build the index as part of the initial
 * sync operation.
 * @tags: [requires_replication]
 */
(function() {
'use strict';

load("jstests/replsets/rslib.js");

const dbName = "test";
const collName = "coll";

const firstIndexName = "_first";

function addTestDocuments(db) {
    let size = 100;
    jsTest.log("Creating " + size + " test documents.");
    var bulk = db.getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i});
    }
    assert.writeOK(bulk.execute());
}

let replSet = new ReplSetTest({name: "indexBuilds", nodes: 2, useBridge: true});
let nodes = replSet.nodeList();

replSet.startSet({startClean: true});
replSet.initiate({
    _id: "indexBuilds",
    members: [
        {_id: 0, host: nodes[0]},
        {_id: 1, host: nodes[1], votes: 0, priority: 0},
    ]
});

let primary = replSet.getPrimary();
let primaryDB = primary.getDB(dbName);

let secondary = replSet.getSecondary();
let secondaryDB = secondary.getDB(dbName);

addTestDocuments(primaryDB);

jsTest.log("Hanging index builds on the secondary node");
assert.commandWorked(secondaryDB.adminCommand(
    {configureFailPoint: "hangAfterStartingIndexBuild", mode: "alwaysOn"}));

jsTest.log("Beginning index build: " + firstIndexName);
assert.commandWorked(primaryDB.runCommand({
    createIndexes: collName,
    indexes: [{key: {i: 1}, name: firstIndexName, background: true}],
    writeConcern: {w: 2}
}));

jsTest.log("Adding a new node to the replica set");
let newNode = replSet.add({rsConfig: {votes: 0, priority: 0}});

// Ensure that the new node and primary cannot communicate to each other.
newNode.disconnect(primary);

replSet.reInitiate();

// Wait for the new node to finish initial sync.
waitForState(newNode, ReplSetTest.State.SECONDARY);

// Let the 'secondary' finish its index build.
jsTest.log("Removing index build hang on the secondary node to allow it to finish");
assert.commandWorked(
    secondaryDB.adminCommand({configureFailPoint: "hangAfterStartingIndexBuild", mode: "off"}));

// Wait for the index builds to finish.
replSet.waitForAllIndexBuildsToFinish(dbName, collName);
jsTest.log("Checking if the indexes match between the new node and the secondary node");

let newNodeDB = newNode.getDB(dbName);
jsTest.log("New nodes indexes:");
printjson(newNodeDB.getCollection(collName).getIndexes());
jsTest.log("Secondary nodes indexes:");
printjson(secondaryDB.getCollection(collName).getIndexes());

assert.eq(newNodeDB.getCollection(collName).getIndexes().length,
          secondaryDB.getCollection(collName).getIndexes().length);

replSet.stopSet();
})();
