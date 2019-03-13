/**
 * Tests the write conflict behavior while the "skipWriteConflictRetries" failpoint is enabled
 * between operations performed outside of a multi-statement transaction when another operation is
 * being performed concurrently inside of a multi-statement transaction.
 *
 * Note that jstests/core/txns/write_conflicts_with_non_txns.js tests the write conflict behavior
 * while the "skipWriteConflictRetries" failpoint isn't enabled.
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
    const testColl = testDB.getCollection("skip_write_conflict_retries_failpoint");

    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(testDB.getName());
    const sessionColl = sessionDB.getCollection(testColl.getName());

    assert.commandWorked(testColl.runCommand(
        "createIndexes",
        {indexes: [{key: {a: 1}, name: "a_1", unique: true}], writeConcern: {w: "majority"}}));

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "skipWriteConflictRetries", mode: "alwaysOn"}));

    // A non-transactional insert would ordinarily keep retrying if it conflicts with a write
    // operation performed inside a multi-statement transaction. However, with the
    // "skipWriteConflictRetries" failpoint enabled, the non-transactional insert should immediately
    // fail with a WriteConflict error response.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "from transaction", a: 0}));

    assert.commandFailedWithCode(testColl.insert({_id: "from outside transaction", a: 0}),
                                 ErrorCodes.WriteConflict);

    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq(testColl.findOne({a: 0}), {_id: "from transaction", a: 0});

    // A non-transactional update would ordinarily keep retrying if it conflicts with a write
    // operation performed inside a multi-statement transaction. However, with the
    // "skipWriteConflictRetries" failpoint enabled, the non-transactional insert should immediately
    // fail with a WriteConflict error response.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "from prepared transaction", a: 1}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    assert.commandFailedWithCode(testColl.update({_id: "from transaction"}, {$set: {a: 1}}),
                                 ErrorCodes.WriteConflict);

    assert.commandWorked(PrepareHelpers.commitTransactionAfterPrepareTS(session, prepareTimestamp));
    assert.eq(testColl.findOne({a: 1}), {_id: "from prepared transaction", a: 1});

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "skipWriteConflictRetries", mode: "off"}));

    session.endSession();
    rst.stopSet();
})();
