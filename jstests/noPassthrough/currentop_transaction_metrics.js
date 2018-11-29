/**
 * Tests that the time-tracking metrics in the 'transaction' object in currentOp() are being tracked
 * correctly.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    'use strict';
    load("jstests/core/txns/libs/prepare_helpers.js");

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const collName = 'currentop_transaction_metrics';
    const testDB = rst.getPrimary().getDB('test');
    const adminDB = rst.getPrimary().getDB('admin');
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB[collName].insert({x: 1}, {writeConcern: {w: "majority"}}));

    const session = adminDB.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase('test');

    session.startTransaction();
    // Run a few operations so that the transaction goes through several active/inactive periods.
    assert.commandWorked(sessionDB[collName].update({}, {a: 1}));
    assert.commandWorked(sessionDB[collName].insert({_id: "insert-1"}));
    assert.commandWorked(sessionDB[collName].insert({_id: "insert-2"}));
    assert.commandWorked(sessionDB[collName].insert({_id: "insert-3"}));

    const transactionFilter = {
        active: false,
        'lsid': {$exists: true},
        'transaction.parameters.txnNumber': {$eq: 0},
        'transaction.parameters.autocommit': {$eq: false},
        'transaction.timePreparedMicros': {$exists: false}
    };

    let currentOp = adminDB.aggregate([{$currentOp: {}}, {$match: transactionFilter}]).toArray();
    assert.eq(currentOp.length, 1);

    // Check that the currentOp's transaction subdocument's fields align with our expectations.
    let transactionDocument = currentOp[0].transaction;
    assert.gte(transactionDocument.timeOpenMicros,
               transactionDocument.timeActiveMicros + transactionDocument.timeInactiveMicros);

    // Check that preparing the transaction enables the 'timePreparedMicros' field in currentOp.
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    const prepareTransactionFilter = {
        active: false,
        'lsid': {$exists: true},
        'transaction.parameters.txnNumber': {$eq: 0},
        'transaction.parameters.autocommit': {$eq: false},
        'transaction.timePreparedMicros': {$exists: true}
    };

    currentOp = adminDB.aggregate([{$currentOp: {}}, {$match: prepareTransactionFilter}]).toArray();
    assert.eq(currentOp.length, 1);

    // Check that the currentOp's transaction subdocument's fields align with our expectations.
    const prepareTransactionDocument = currentOp[0].transaction;
    assert.gte(prepareTransactionDocument.timeOpenMicros,
               prepareTransactionDocument.timeActiveMicros +
                   prepareTransactionDocument.timeInactiveMicros);
    assert.gte(prepareTransactionDocument.timePreparedMicros, 0);

    PrepareHelpers.commitTransactionAfterPrepareTS(session, prepareTimestamp);
    session.endSession();

    rst.stopSet();
})();
