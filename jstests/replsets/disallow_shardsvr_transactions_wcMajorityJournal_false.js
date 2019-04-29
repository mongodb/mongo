/**
 * Test that transactions are not allowed on shard servers that have
 * writeConcernMajorityJournalDefault = false.
 *
 * @tags: [uses_transactions]
 */

(function() {
    "use strict";

    // A testing exemption was made to allow transactions on shard server even if
    // writeConcernMajorityJournalDefault = false. So we need to disable the exemption in this test
    // in order to test the behavior.
    jsTest.setOption('enableTestCommands', false);

    // The following two options by default do not support enableTestCommands=false, change them
    // accordingly so this test can run.
    TestData.roleGraphInvalidationIsFatal = false;
    TestData.authenticationDatabase = "local";

    // Start the replica set with --shardsvr.
    const replSet = new ReplSetTest({nodes: 1, nodeOptions: {shardsvr: ""}});
    replSet.startSet();
    let conf = replSet.getReplSetConfig();
    conf.writeConcernMajorityJournalDefault = false;
    replSet.initiate(conf);

    const primary = replSet.getPrimary();
    const session = primary.startSession();
    const sessionDb = session.getDatabase("test");
    const sessionColl = sessionDb.getCollection("foo");

    jsTestLog("Test that non-transactional operations are allowed.");
    assert.commandWorked(sessionColl.insert({_id: 1}));

    jsTestLog("Test that transactions are not allowed.");
    session.startTransaction();
    assert.commandFailedWithCode(sessionColl.insert({_id: 2}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    // All commands are not allowed including abortTransaction.
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.OperationNotSupportedInTransaction);

    jsTestLog("Test that retryable writes are allowed.");
    assert.commandWorked(
        sessionDb.runCommand({insert: "foo", documents: [{_id: 3}], txnNumber: NumberLong(1)}));

    // Assert documents inserted.
    assert.docEq(sessionColl.find().sort({_id: 1}).toArray(), [{_id: 1}, {_id: 3}]);

    replSet.stopSet();
}());
