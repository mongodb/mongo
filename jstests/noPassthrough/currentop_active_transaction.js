/**
 * Confirms inclusion of a 'transaction' object containing lsid and txnNumber in
 * currentOp() for a prepared transaction and an active non-prepared transaction.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    'use strict';
    load("jstests/libs/parallel_shell_helpers.js");

    function transactionFn(isPrepared) {
        const collName = 'currentop_active_transaction';
        const session = db.getMongo().startSession({causalConsistency: false});
        const sessionDB = session.getDatabase('test');

        session.startTransaction({readConcern: {level: 'snapshot'}});
        sessionDB[collName].update({}, {x: 2});
        if (isPrepared) {
            // Load the prepare helpers to be called in the parallel shell.
            load('jstests/core/txns/libs/prepare_helpers.js');
            const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
            PrepareHelpers.commitTransaction(session, prepareTimestamp);
        } else {
            session.commitTransaction();
        }
    }

    function checkCurrentOpFields(currentOp,
                                  isPrepared,
                                  operationTime,
                                  timeBeforeTransactionStarts,
                                  timeAfterTransactionStarts,
                                  timeBeforeCurrentOp) {
        const transactionDocument = currentOp[0].transaction;
        assert.eq(transactionDocument.parameters.autocommit,
                  false,
                  "Expected 'autocommit' to be false but got " +
                      transactionDocument.parameters.autocommit + " instead: " +
                      tojson(transactionDocument));
        assert.docEq(transactionDocument.parameters.readConcern,
                     {level: 'snapshot'},
                     "Expected 'readConcern' to be level: snapshot but got " +
                         tojson(transactionDocument.parameters.readConcern) + " instead: " +
                         tojson(transactionDocument));
        assert.gte(transactionDocument.readTimestamp,
                   operationTime,
                   "Expected 'readTimestamp' to be at least " + operationTime + " but got " +
                       transactionDocument.readTimestamp + " instead: " +
                       tojson(transactionDocument));
        assert.gte(ISODate(transactionDocument.startWallClockTime),
                   timeBeforeTransactionStarts,
                   "Expected 'startWallClockTime' to be at least" + timeBeforeTransactionStarts +
                       " but got " + transactionDocument.startWallClockTime + " instead: " +
                       tojson(transactionDocument));
        const expectedTimeOpen = (timeBeforeCurrentOp - timeAfterTransactionStarts) * 1000;
        assert.gt(transactionDocument.timeOpenMicros,
                  expectedTimeOpen,
                  "Expected 'timeOpenMicros' to be at least" + expectedTimeOpen + " but got " +
                      transactionDocument.timeOpenMicros + " instead: " +
                      tojson(transactionDocument));
        assert.gte(transactionDocument.timeActiveMicros,
                   0,
                   "Expected 'timeActiveMicros' to be at least 0: " + tojson(transactionDocument));
        assert.gte(
            transactionDocument.timeInactiveMicros,
            0,
            "Expected 'timeInactiveMicros' to be at least 0: " + tojson(transactionDocument));
        const actualExpiryTime = ISODate(transactionDocument.expiryTime).getTime();
        const expectedExpiryTime =
            ISODate(transactionDocument.startWallClockTime).getTime() + transactionLifeTime * 1000;
        assert.eq(expectedExpiryTime,
                  actualExpiryTime,
                  "Expected 'expiryTime' to be " + expectedExpiryTime + " but got " +
                      actualExpiryTime + " instead: " + tojson(transactionDocument));
        if (isPrepared) {
            assert.gte(
                transactionDocument.timePreparedMicros,
                0,
                "Expected 'timePreparedMicros' to be at least 0: " + tojson(transactionDocument));
        }
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const collName = 'currentop_active_transaction';
    const testDB = rst.getPrimary().getDB('test');
    const adminDB = rst.getPrimary().getDB('admin');
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB[collName].insert({x: 1}, {writeConcern: {w: "majority"}}));

    // Run an operation prior to starting the transaction and save its operation time. We will use
    // this later to assert that our subsequent transaction's readTimestamp is greater than or equal
    // to this operation time.
    let res = assert.commandWorked(testDB.runCommand({insert: collName, documents: [{x: 1}]}));

    // Set and save the transaction's lifetime. We will use this later to assert that our
    // transaction's expiry time is equal to its start time + lifetime.
    const transactionLifeTime = 10;
    assert.commandWorked(testDB.adminCommand(
        {setParameter: 1, transactionLifetimeLimitSeconds: transactionLifeTime}));

    // This will make the transaction hang.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'hangAfterSettingPrepareStartTime', mode: 'alwaysOn'}));

    let timeBeforeTransactionStarts = new ISODate();
    let isPrepared = true;
    const joinPreparedTransaction =
        startParallelShell(funWithArgs(transactionFn, isPrepared), rst.ports[0]);

    const prepareTransactionFilter = {
        active: true,
        'lsid': {$exists: true},
        'transaction.parameters.txnNumber': {$eq: 0},
        'transaction.parameters.autocommit': {$eq: false},
        'transaction.timePreparedMicros': {$exists: true}
    };

    // Keep running currentOp() until we see the transaction subdocument.
    assert.soon(function() {
        return 1 ===
            adminDB.aggregate([{$currentOp: {}}, {$match: prepareTransactionFilter}]).itcount();
    });

    let timeAfterTransactionStarts = new ISODate();
    // Sleep here to allow some time between timeAfterTransactionStarts and timeBeforeCurrentOp to
    // elapse.
    sleep(100);
    let timeBeforeCurrentOp = new ISODate();
    // Check that the currentOp's transaction subdocument's fields align with our expectations.
    let currentOp =
        adminDB.aggregate([{$currentOp: {}}, {$match: prepareTransactionFilter}]).toArray();
    checkCurrentOpFields(currentOp,
                         isPrepared,
                         res.operationTime,
                         timeBeforeTransactionStarts,
                         timeAfterTransactionStarts,
                         timeBeforeCurrentOp);

    // Now the transaction can proceed.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangAfterSettingPrepareStartTime', mode: 'off'}));
    joinPreparedTransaction();

    // Conduct the same test but with a non-prepared transaction.
    res = assert.commandWorked(testDB.runCommand({insert: collName, documents: [{x: 1}]}));

    // This will make the transaction hang.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangDuringBatchUpdate', mode: 'alwaysOn'}));

    timeBeforeTransactionStarts = new ISODate();
    isPrepared = false;
    const joinTransaction =
        startParallelShell(funWithArgs(transactionFn, isPrepared), rst.ports[0]);

    const transactionFilter = {
        active: true,
        'lsid': {$exists: true},
        'transaction.parameters.txnNumber': {$eq: 0},
        'transaction.parameters.autocommit': {$eq: false},
        'transaction.timePreparedMicros': {$exists: false}
    };

    // Keep running currentOp() until we see the transaction subdocument.
    assert.soon(function() {
        return 1 === adminDB.aggregate([{$currentOp: {}}, {$match: transactionFilter}]).itcount();
    });

    timeAfterTransactionStarts = new ISODate();
    // Sleep here to allow some time between timeAfterTransactionStarts and timeBeforeCurrentOp to
    // elapse.
    sleep(100);
    timeBeforeCurrentOp = new ISODate();
    // Check that the currentOp's transaction subdocument's fields align with our expectations.
    currentOp = adminDB.aggregate([{$currentOp: {}}, {$match: transactionFilter}]).toArray();
    checkCurrentOpFields(currentOp,
                         isPrepared,
                         res.operationTime,
                         timeBeforeTransactionStarts,
                         timeAfterTransactionStarts,
                         timeBeforeCurrentOp);

    // Now the transaction can proceed.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangDuringBatchUpdate', mode: 'off'}));
    joinTransaction();

    rst.stopSet();
})();
