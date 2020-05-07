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

const replSet = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
    ],
    useBridge: true,
});
const nodes = replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let primaryDB = primary.getDB(dbName);

let secondary = replSet.getSecondary();
let secondaryDB = secondary.getDB(dbName);

addTestDocuments(primaryDB);

// Used to wait for two-phase builds to complete.
let awaitIndex;

jsTest.log("Hanging index build on the primary node");
IndexBuildTest.pauseIndexBuilds(primary);

jsTest.log("Beginning index build");
const coll = primaryDB.getCollection(collName);
awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {i: 1});

jsTest.log("Waiting for index build to start on secondary");
IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

jsTest.log("Adding a new node to the replica set");
let newNode = replSet.add({
    // Disallow elections on secondary.
    rsConfig: {
        priority: 0,
        votes: 0,
    },
    slowms: 30000,  // Don't log slow operations on secondary.
});

// Ensure that the new node and primary cannot communicate to each other.
newNode.disconnect(primary);

replSet.reInitiate();

// Wait for the new node to finish initial sync.
waitForState(newNode, ReplSetTest.State.SECONDARY);

jsTest.log("Removing index build hang to allow it to finish");
IndexBuildTest.resumeIndexBuilds(primary);
awaitIndex();

// Wait for the index builds to finish.
replSet.awaitReplication();
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
