/**
 * Tests that the 'prepareTransaction', 'commitTransaction', and 'abortTransaction' fail on
 * secondaries.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const name = "prepared_transaction_commands_fail_on_secondaries";
    const rst = new ReplSetTest({
        nodes: [
            {},
            {rsConfig: {priority: 0}},
        ],
    });
    const nodes = rst.startSet();
    rst.initiate();

    const dbName = "test";
    const collName = name;

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const testDB = primary.getDB(dbName);

    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const priSession = primary.startSession({causalConsistency: false});
    const priSessionDB = priSession.getDatabase(dbName);
    const priSessionColl = priSessionDB.getCollection(collName);

    const secSession = PrepareHelpers.createSessionWithGivenId(
        secondary, priSession.getSessionId(), {causalConsistency: false});

    priSession.startTransaction();
    const doc = {_id: 1};
    assert.commandWorked(priSessionColl.insert(doc));
    rst.awaitReplication();

    jsTestLog("Test that prepare fails on a secondary");
    const txnNumber = NumberLong(priSession.getTxnNumber_forTesting());
    assert.commandFailedWithCode(
        secSession.getDatabase('admin').adminCommand(
            {prepareTransaction: 1, txnNumber: txnNumber, autocommit: false}),
        ErrorCodes.NotMaster);

    const prepareTimestamp = PrepareHelpers.prepareTransaction(priSession);
    rst.awaitReplication();

    jsTestLog("Test that prepared commit fails on a secondary");
    // Add 1 to the increment so that the commitTimestamp is "after" the prepareTimestamp.
    const commitTimestamp = Timestamp(prepareTimestamp.getTime(), prepareTimestamp.getInc() + 1);
    assert.commandFailedWithCode(secSession.getDatabase('admin').adminCommand({
        commitTransaction: 1,
        commitTimestamp: commitTimestamp,
        txnNumber: txnNumber,
        autocommit: false
    }),
                                 ErrorCodes.NotMaster);

    jsTestLog("Test that prepared abort fails on a secondary");
    assert.commandFailedWithCode(
        secSession.getDatabase('admin').adminCommand(
            {abortTransaction: 1, txnNumber: txnNumber, autocommit: false}),
        ErrorCodes.NotMaster);

    jsTestLog("Test that we can still commit the transaction");
    assert.commandWorked(PrepareHelpers.commitTransaction(priSession, commitTimestamp));
    rst.awaitReplication();
    assert.docEq(doc, testDB[collName].findOne());
    assert.eq(1, testDB[collName].find().itcount());
    assert.docEq(doc, secondary.getDB(dbName)[collName].findOne());
    assert.eq(1, secondary.getDB(dbName)[collName].find().itcount());

    rst.stopSet();
})();
