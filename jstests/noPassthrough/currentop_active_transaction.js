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
    const transactionProcess = startParallelShell(transactionFn, rst.ports[0]);

    // Keep running currentOp() until we see the transaction subdocument.
    assert.soon(function() {
        const transactionFilter =
            {active: true, 'lsid': {$exists: true}, 'transaction.parameters.txnNumber': {$eq: 0}};
        return 1 === adminDB.aggregate([{$currentOp: {}}, {$match: transactionFilter}]).itcount();
    });

    // Now the transaction can proceed.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'setInterruptOnlyPlansCheckForInterruptHang', mode: 'off'}));
    transactionProcess();

    rst.stopSet();
})();
