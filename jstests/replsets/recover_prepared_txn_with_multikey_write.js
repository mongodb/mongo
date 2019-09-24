/**
 * Test that replication recovery can reconstruct a prepared transaction that includes a write that
 * sets the multikey flag.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, requires_persistence]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            }
        }
    ]
});

rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

const session = primary.getDB("test").getMongo().startSession();
const sessionDB = session.getDatabase("test");
const sessionColl = sessionDB.getCollection("coll");

// Create an index that will later be made multikey.
sessionColl.createIndex({x: 1});
session.startTransaction();

// Make the index multikey.
jsTestLog("Making the index multikey.");
sessionColl.insert({x: [1, 2, 3]});
// Make sure { w: "majority" } is always used, otherwise the prepare may not get journaled before
// the shutdown below.
PrepareHelpers.prepareTransaction(session);

// Do an unclean shutdown so we don't force a checkpoint, and then restart.
jsTestLog("Killing the primary.");
rst.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
rst.restart(0);

jsTestLog("Waiting for the node to get elected again.");
primary = rst.getPrimary();

jsTestLog("Making sure no prepare conflicts are generated on the catalog.");
assert.commandWorked(primary.adminCommand({listDatabases: 1}));

jsTestLog("Aborting the prepared transaction.");
assert.commandWorked(primary.adminCommand({
    abortTransaction: 1,
    lsid: session.getSessionId(),
    txnNumber: session.getTxnNumber_forTesting(),
    autocommit: false
}));

rst.stopSet();
}());
