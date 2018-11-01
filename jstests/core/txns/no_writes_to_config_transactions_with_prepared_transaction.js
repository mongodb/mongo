/**
 * Tests that other than insertions, it is illegal to modify config.transactions while the session
 * has a prepared transaction.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    TestData.disableImplicitSessions = true;

    const dbName = "test";
    const collName = "no_writes_to_config_transactions_with_prepared_transaction";
    const collName2 = "no_writes_to_config_transactions_with_prepared_transaction2";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    const config = db.getSiblingDB("config");
    const transactionsColl = config.getCollection("transactions");

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    testDB.runCommand({drop: collName2, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName2, writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    const sessionConfigDB = session.getDatabase("config");

    // Start a transaction using runCommand so that we can run commands on the session but outside
    // the transaction.
    assert.commandWorked(sessionDB.runCommand({
        insert: collName,
        documents: [{_id: 1}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(0),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(sessionDB.adminCommand({
        prepareTransaction: 1,
        txnNumber: NumberLong(0),
        stmtId: NumberInt(1),
        autocommit: false
    }));

    let transactionEntry = config.transactions.findOne();
    const txnNum = transactionEntry.txnNum;

    jsTestLog("Test that updates to config.transactions fails when there is a prepared " +
              "transaction on the session");
    assert.commandFailedWithCode(
        sessionConfigDB.transactions.update({_id: transactionEntry._id},
                                            {$set: {"txnNumber": NumberLong(23)}}),
        40528);

    // Make sure that the txnNumber wasn't modified.
    transactionEntry = config.transactions.findOne();
    assert.eq(transactionEntry.txnNum, NumberLong(txnNum));

    jsTestLog("Test that deletes to config.transactions fails when there is a prepared " +
              "transaction on the session");
    assert.commandFailedWithCode(sessionConfigDB.transactions.remove({_id: transactionEntry._id}),
                                 40528);

    // Make sure that the entry in config.transactions wasn't removed.
    transactionEntry = config.transactions.findOne();
    assert(transactionEntry);

    jsTestLog("Test that dropping config.transactions fails when there is a prepared transaction" +
              " on the session");
    assert.commandFailedWithCode(assert.throws(function() {
        sessionConfigDB.transactions.drop();
    }),
                                 40528);

    jsTestLog("Test that we can prepare a transaction on a different session");
    const session2 = db.getMongo().startSession({causalConsistency: false});
    const sessionDB2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDB2.getCollection(collName2);

    session2.startTransaction();
    assert.commandWorked(sessionColl2.insert({_id: 1}));
    // This will cause an insertion into config.transactions
    PrepareHelpers.prepareTransaction(session2);

    assert.commandWorked(sessionDB.adminCommand(
        {abortTransaction: 1, txnNumber: NumberLong(0), stmtid: NumberInt(2), autocommit: false}));
    session.endSession();

    session2.abortTransaction_forTesting();
    session2.endSession();

}());
