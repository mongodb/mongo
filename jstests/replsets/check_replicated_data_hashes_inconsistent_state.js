/*
 * Tests that checkReplicatedDataHashes will fail when there exists a data inconsistency between the
 * primary and secondary.
 *
 * @tags: [uses_testing_only_commands]
 */

(function() {
"use strict";

// Skip DB hash check in stopSet() since we expect it to fail in this test.
TestData.skipCheckDBHashes = true;

const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB0 = primary.getDB("db0");
const primaryDB1 = primary.getDB("db1");
const primaryDB2 = primary.getDB("db2");
const collName = "testColl";

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB("db0");

// Create a collection on the primary, so that the secondary will have this collection.
assert.commandWorked(primaryDB0.createCollection(collName));
rst.awaitReplication();

// Insert a write on the secondary that will not be on the primary.
assert.commandWorked(secondaryDB.runCommand({godinsert: collName, obj: {_id: 0, a: 0}}));

// Insert writes on various DBs that should be replicated.
assert.commandWorked(primaryDB1.runCommand({insert: collName, documents: [{_id: 1, b: 1}]}));
assert.commandWorked(primaryDB2.runCommand({insert: collName, documents: [{_id: 2, c: 2}]}));

const err = assert.throws(() => rst.checkReplicatedDataHashes());
assert(err.message.includes("dbhash mismatch between primary and secondary"),
       `caught error didn't mention dbhash mismatch: ${tojson(err)}`);

rst.stopSet();
})();
