/**
 * Tests the WT prepare conflict behavior of running operations outside of a multi-statement
 * transaction when another operation is being performed concurrently inside of the multi-statement
 * transaction with the "WTSkipPrepareConflictRetries" failpoint is enabled.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const testColl = testDB.getCollection("wt_skip_prepare_conflict_retries_failpoint");

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(testDB.getName());
const sessionColl = sessionDB.getCollection(testColl.getName());

assert.commandWorked(testDB.runCommand({profile: 2}));

assert.commandWorked(testColl.insert({_id: 1, note: "from before transaction"}, {w: "majority"}));

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "WTSkipPrepareConflictRetries", mode: "alwaysOn"}));

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "skipWriteConflictRetries", mode: "alwaysOn"}));

// A non-transactional operation conflicting with a write operation performed inside a
// multistatement transaction can encounter a WT_PREPARE_CONFLICT in the wiredtiger
// layer under several circumstances, such as performing an insert, update, or find
// on a document that is in a prepare statement. The non-transactional operation
// would then be retried after the prepared transaction commits or aborts. However, with the
// "WTSkipPrepareConflictRetries"failpoint enabled, the non-transactional operation would
// instead return with a WT_ROLLBACK error. This would then get bubbled up as a
// WriteConflictException. Enabling the "skipWriteConflictRetries" failpoint then prevents
// the higher layers from retrying the entire operation.
session.startTransaction();

assert.commandWorked(sessionColl.update({_id: 1}, {$set: {note: "from prepared transaction"}}));

const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

assert.commandFailedWithCode(
    testColl.update({_id: 1}, {$set: {note: "outside prepared transaction"}}),
    ErrorCodes.WriteConflict);

assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

const profileEntry =
    testDB.system.profile.findOne({"command.u.$set.note": "outside prepared transaction"});
assert.gte(profileEntry.prepareReadConflicts, 1);

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "WTSkipPrepareConflictRetries", mode: "off"}));

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "skipWriteConflictRetries", mode: "off"}));

session.endSession();
rst.stopSet();
})();
