/**
 * Tests that a background index will be successfully
 *  replicated to a secondary when the indexed collection
 *  is renamed.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Bring up a 2 node replset.
let name = "bg_index_rename";
let rst = new ReplSetTest({name: name, nodes: 3});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

// Create and populate a collection.
let primary = rst.getPrimary();
let coll = primary.getCollection("test.foo");
let adminDB = primary.getDB("admin");

for (let i = 0; i < 100; i++) {
    assert.commandWorked(coll.insert({_id: i, x: i * 3, str: "hello world"}));
}

// Add a background index.
coll.createIndex({x: 1});

// Rename the collection.
assert.commandWorked(
    adminDB.runCommand({renameCollection: "test.foo", to: "bar.test", dropTarget: true}),
    "Call to renameCollection failed.",
);

// Await replication.
rst.awaitReplication();

// Step down the primary.
assert.commandWorked(adminDB.runCommand({replSetStepDown: 60, force: true}));

// Wait for new primary.
let newPrimary = rst.getPrimary();
assert.neq(primary, newPrimary);
let barDB = newPrimary.getDB("bar");
coll = newPrimary.getCollection("bar.test");
coll.insert({_id: 200, x: 600, str: "goodnight moon"});

// Check that the new primary has the index
// on the renamed collection.
let indexes = barDB.runCommand({listIndexes: "test"});
assert.eq(indexes.cursor.firstBatch.length, 2);

rst.stopSet();
