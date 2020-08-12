/**
 * Test that initial sync can reconstruct a prepared transaction that includes a write that
 * sets the multikey flag.
 *
 * @tags: [
 *  uses_transactions,
 *  uses_prepare_transaction,
 *  requires_persistence,
 *  # Multiversion testing does not support tests that kill and restart nodes.
 *  multiversion_incompatible
 * ]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const rst = new ReplSetTest({nodes: 1});

rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

const primaryDB = primary.getDB("test");
const session = primaryDB.getMongo().startSession();
const sessionDB = session.getDatabase("test");
const sessionColl = sessionDB.getCollection("coll");

// Create an index that will later be made multikey.
sessionColl.createIndex({x: 1});
session.startTransaction();

// Make the index multikey.
jsTestLog("Making the index multikey.");
sessionColl.insert({x: [1, 2, 3]});
let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

jsTestLog("Doing another write outside of transaction.");
assert.commandWorked(primaryDB.runCommand({insert: "coll", documents: [{x: 4}]}));

jsTestLog("Adding a secondary node to do the initial sync.");
rst.add();

jsTestLog("Re-initiating replica set with the new secondary.");
rst.reInitiate();

// Wait until initial sync completes.
jsTestLog("Waiting until initial sync completes.");
rst.awaitSecondaryNodes();
rst.awaitReplication();

jsTestLog("Committing the prepared transaction.");
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

rst.stopSet();
}());
