/**
 * Tests that 'prepareTransaction' only succeeds in FCV 4.2.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "prepare_requires_fcv42";
    const testDB = db.getSiblingDB(dbName);
    const adminDB = db.getSiblingDB('admin');

    testDB[collName].drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDB = session.getDatabase(dbName);

    try {
        jsTestLog("Transaction succeeds in latest FCV.");
        checkFCV(adminDB, latestFCV);
        session.startTransaction();
        assert.commandWorked(sessionDB[collName].insert({_id: "a"}));
        let prepareTimestamp = PrepareHelpers.prepareTransaction(session);
        assert.commandWorked(
            PrepareHelpers.commitTransactionAfterPrepareTS(session, prepareTimestamp));

        jsTestLog("Downgrade the featureCompatibilityVersion.");
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
        checkFCV(adminDB, lastStableFCV);

        jsTestLog("Transaction fails to prepare in last stable FCV.");
        session.startTransaction();
        assert.commandWorked(sessionDB[collName].insert({_id: "b"}));
        assert.commandFailedWithCode(sessionDB.adminCommand({prepareTransaction: 1}),
                                     ErrorCodes.CommandNotSupported);
        // Abort the transaction in the shell.
        session.abortTransaction_forTesting();

    } finally {
        jsTestLog("Restore the original featureCompatibilityVersion.");
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);
    }

    jsTestLog("Transaction succeeds in latest FCV after upgrade.");
    session.startTransaction();
    assert.commandWorked(sessionDB[collName].insert({_id: "c"}));
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransactionAfterPrepareTS(session, prepareTimestamp));

    session.endSession();
}());
