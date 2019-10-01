/**
 * Test initial sync cloning of a collection that contains a multikey index when hybrid index builds
 * are disabled.
 */
(function() {
"use strict";

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const dbName = jsTest.name();
const collName = "test";

const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);

jsTestLog("Creating the collection and an index.");
assert.commandWorked(primaryDB.createCollection(collName));
assert.commandWorked(primaryDB[collName].createIndex({"x": 1}, {background: true}));

// Make the index multikey.
primaryDB[collName].insert({x: [1, 2]});

jsTestLog("Adding a secondary node to do the initial sync.");
replTest.add({setParameter: "enableHybridIndexBuilds=false"});

jsTestLog("Re-initiating replica set with the new secondary.");
replTest.reInitiate();

// Wait until initial sync completes.
jsTestLog("Waiting until initial sync completes.");
replTest.awaitSecondaryNodes();
replTest.awaitReplication();
replTest.stopSet();
})();