/**
 * Confirms inclusion of a 'transaction' object containing lsid and txnNumber in
 * currentOp() for active transaction.
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

    // This will make the transaction hang.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'setInterruptOnlyPlansCheckForInterruptHang', mode: 'alwaysOn'}));
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

    const transactionFn = function() {
        const collName = 'currentop_active_transaction';
        const session = db.getMongo().startSession({causalConsistency: false});
        const sessionDB = session.getDatabase('test');

        session.startTransaction({readConcern: {level: 'snapshot'}});
        sessionDB[collName].update({}, {x: 2});
        session.commitTransaction();
    };

    const timeBeforeTransactionStarts = new ISODate();
    const transactionProcess = startParallelShell(transactionFn, rst.ports[0]);

    const transactionFilter = {
        active: true,
        'lsid': {$exists: true},
        'transaction.parameters.txnNumber': {$eq: 0},
        'transaction.parameters.autocommit': {$eq: false}
    };

    // Keep running currentOp() until we see the transaction subdocument.
    assert.soon(function() {
        return 1 === adminDB.aggregate([{$currentOp: {}}, {$match: transactionFilter}]).itcount();
    });

    const timeAfterTransactionStarts = new ISODate();
    // Sleep here to allow some time between timeAfterTransactionStarts and timeBeforeCurrentOp to
    // elapse.
    sleep(100);
    const timeBeforeCurrentOp = new ISODate();
    // Check that the currentOp's transaction subdocument's fields align with our expectations.
    let currentOp = adminDB.aggregate([{$currentOp: {}}, {$match: transactionFilter}]).toArray();
    let transactionDocument = currentOp[0].transaction;
    assert.eq(transactionDocument.parameters.autocommit, false);
    assert.gt(transactionDocument.timeOpenMicros,
              (timeBeforeCurrentOp - timeAfterTransactionStarts) * 1000);
    assert.gte(ISODate(transactionDocument.startWallClockTime), timeBeforeTransactionStarts);

    // Now the transaction can proceed.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'setInterruptOnlyPlansCheckForInterruptHang', mode: 'off'}));
    transactionProcess();

    rst.stopSet();
})();
