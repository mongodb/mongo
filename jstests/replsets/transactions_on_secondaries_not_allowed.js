/**
 * Test that starting transactions and running commitTransaction and abortTransaction commands are
 * not allowed on replica set secondaries.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "transactions_on_secondaries_not_allowed";

    const rst = new ReplSetTest({name: collName, nodes: 2});
    rst.startSet({verbose: 3});
    // We want a stable topology, so make the secondary unelectable.
    let config = rst.getReplSetConfig();
    config.members[1].priority = 0;
    rst.initiate(config);

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const secondaryTestDB = secondary.getDB(dbName);

    // Do an initial write so we have something to find.
    const initialDoc = {_id: 0};
    assert.commandWorked(primary.getDB(dbName)[collName].insert(initialDoc));
    rst.awaitLastOpCommitted();

    // Disable the best-effort check for primary-ness in the service entry point, so that we
    // exercise the real check for primary-ness in TransactionParticipant::beginOrContinue.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: "skipCheckingForNotMasterInCommandDispatch", mode: "alwaysOn"}));

    // Initiate a session on the secondary.
    const sessionOptions = {causalConsistency: false, retryWrites: true};
    const session = secondaryTestDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    /**
     * Test starting a transaction and issuing a commitTransaction command.
     */

    jsTestLog("Start a read-only transaction on the secondary.");
    session.startTransaction({readConcern: {level: "snapshot"}});

    // Try to read a document (the first statement in the transaction) and verify that this fails.
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.NotMaster);

    // The check for "NotMaster" supercedes the check for "NoSuchTransaction" in this case.
    jsTestLog(
        "Make sure we are not allowed to run the commitTransaction command on the secondary.");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.NotMaster);

    /**
     * Test starting a transaction and issuing an abortTransaction command.
     */

    jsTestLog("Start a different read-only transaction on the secondary.");
    session.startTransaction({readConcern: {level: "snapshot"}});

    // Try to read a document (the first statement in the transaction) and verify that this fails.
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.NotMaster);

    // The check for "NotMaster" supercedes the check for "NoSuchTransaction" in this case.
    jsTestLog("Make sure we are not allowed to run the abortTransaction command on the secondary.");
    assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NotMaster);

    /**
     * Test starting a retryable write.
     */

    jsTestLog("Start a retryable write");
    assert.commandFailedWithCode(sessionDb.foo.insert({_id: 0}), ErrorCodes.NotMaster);

    /**
     * Test starting a read with txnNumber, but without autocommit. This is not an officially
     * supported combination, but should still be disallowed on a secondary.
     */

    jsTestLog("Start a read with txnNumber but without autocommit");
    assert.commandFailedWithCode(sessionDb.runCommand({find: 'foo', txnNumber: NumberLong(10)}),
                                 ErrorCodes.NotMaster);

    session.endSession();
    rst.stopSet(undefined, false, {skipValidation: true});
}());
