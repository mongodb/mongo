/**
 * Tests that the time-tracking metrics in the 'transaction' object in currentOp() are being tracked
 * correctly.
 * @tags: [uses_transactions]
 */

(function() {
    'use strict';

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const collName = 'currentop_active_transaction';
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
    };

    let currentOp = adminDB.aggregate([{$currentOp: {}}, {$match: transactionFilter}]).toArray();
    assert.eq(currentOp.length, 1);

    // Check that the currentOp's transaction subdocument's fields align with our expectations.
    let transactionDocument = currentOp[0].transaction;
    assert.gte(transactionDocument.timeOpenMicros,
               transactionDocument.timeActiveMicros + transactionDocument.timeInactiveMicros);

    session.commitTransaction();
    session.endSession();

    rst.stopSet();
})();
