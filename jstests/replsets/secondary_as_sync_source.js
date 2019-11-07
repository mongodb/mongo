/**
 * Tests that a new replica set member performing an initial sync from a secondary node as the sync
 * source, which has an in-progress index build will also build the index as part of the initial
 * sync operation.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');
load("jstests/replsets/rslib.js");

const dbName = "test";
const collName = "coll";

function addTestDocuments(db) {
    let size = 100;
    jsTest.log("Creating " + size + " test documents.");
    var bulk = db.getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i});
    }
    assert.commandWorked(bulk.execute());
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

// Used to wait for two-phase builds to complete.
let awaitIndex;

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    jsTest.log("Hanging index build on the secondary node");
    IndexBuildTest.pauseIndexBuilds(secondary);

    jsTest.log("Beginning index build");
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {i: 1}, name: "i_1"}],
        writeConcern: {w: 2},
    }));
} else {
    jsTest.log("Hanging index build on the primary node");
    IndexBuildTest.pauseIndexBuilds(primary);

    jsTest.log("Beginning index build");
    const coll = primaryDB.getCollection(collName);
    awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {i: 1});

    jsTest.log("Waiting for index build to start on secondary");
    IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
}

jsTest.log("Adding a new node to the replica set");
let newNode = replSet.add({rsConfig: {votes: 0, priority: 0}});

// Ensure that the new node and primary cannot communicate to each other.
newNode.disconnect(primary);

replSet.reInitiate();

// Wait for the new node to finish initial sync.
waitForState(newNode, ReplSetTest.State.SECONDARY);

jsTest.log("Removing index build hang to allow it to finish");
if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    // Let the 'secondary' finish its index build.
    IndexBuildTest.resumeIndexBuilds(secondary);
} else {
    IndexBuildTest.resumeIndexBuilds(primary);
    awaitIndex();
}

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
