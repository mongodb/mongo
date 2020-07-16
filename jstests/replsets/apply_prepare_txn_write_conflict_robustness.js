/*
 * Tests that WT_ROLLBACK (WriteConflictException) error gets retried when applying
 * prepareTransaction oplog entry on secondaries.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {

"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const dbName = jsTest.name();
const collName = "coll";

var rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

jsTestLog("Do a document write");
assert.commandWorked(
        primaryColl.insert({_id: 0}, {"writeConcern": {"w": "majority"}}));

// Enable fail point on secondary to cause apply prepare transaction oplog entry's ops to fail
// with write conflict error at least once.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "applyPrepareTxnOpsFailsWithWriteConflict", mode: {times: 1}}));

jsTestLog("Start transaction");
let session = primary.startSession();
let sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(sessionColl.insert({_id: 1}));

// PrepareTransaction cmd will be successful only if secondary is able to retry applying
// prepareTransaction oplog entry on WT_ROLLBACK (WriteConflictException) error.
jsTestLog("Prepare transaction");
let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

jsTestLog("Commit transaction");
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

// Verify that the committed  transaction data is present on secondary.
assert.eq(secondary.getDB(dbName)[collName].findOne({_id: 1}), {_id: 1});

// verify that secondaries are not holding any transactional lock resources.
primaryColl.drop();
rst.awaitReplication();

rst.stopSet();
})();
