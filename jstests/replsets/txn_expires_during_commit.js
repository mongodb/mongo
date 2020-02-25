/**
 * Tests that a transaction will be aborted properly if it expires in the middle of a
 * commitTransaction.
 *
 * @tags: [uses_transactions]
 */

(function() {
    "use strict";

    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: 5}));

    const session = primary.getDB("test").getMongo().startSession();
    const sessionDb = session.getDatabase("test");
    assert.commandWorked(sessionDb.createCollection("c"));

    // Hang in the middle of the commitTransaction command, before the transaction is committed on
    // the session.
    assert.commandWorked(sessionDb.adminCommand({
        configureFailPoint: "sleepBeforeCommitTransaction",
        mode: "alwaysOn",
        data: {sleepMillis: NumberInt(999999)}
    }));
    session.startTransaction();
    assert.commandWorked(sessionDb.c.insert({a: 1}));
    // We expect the commitTransction operation to be killed and that the transaction is aborted
    // cleanly.
    assert.commandFailedWithCode(
        sessionDb.adminCommand({commitTransaction: 1, writeConcern: {w: "majority"}}),
        ErrorCodes.ExceededTimeLimit);

    // Update the transaction state in the shell.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    replTest.stopSet();
})();
