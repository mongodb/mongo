/**
 * Test that we wait for prepared transactions to finish during downgrade to FCV 4.0.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/libs/feature_compatibility_version.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "await_prepared_transactions_on_FCV_downgrade";
    const testDB = db.getSiblingDB(dbName);
    const adminDB = db.getSiblingDB("admin");

    testDB[collName].drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);

    try {
        jsTestLog("Start a transaction.");
        session.startTransaction();
        assert.commandWorked(sessionDB[collName].insert({"a": 1}));

        jsTestLog("Put that transaction into a prepared state.");
        let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        // The setFCV command will need to acquire a global S lock to complete. The global
        // lock is currently held by prepare, so that will block. We use a failpoint to make that
        // command fail immediately when it tries to get the lock.
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "alwaysOn"}));

        jsTestLog("Attempt to downgrade the featureCompatibilityVersion.");
        assert.commandFailedWithCode(
            testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}),
            ErrorCodes.LockTimeout);

        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "off"}));

        jsTestLog("Verify that the setFCV command set the target version to 'lastStable'.");
        checkFCV(adminDB, lastStableFCV, lastStableFCV);

        jsTestLog("Commit the prepared transaction.");
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

        jsTestLog("Rerun the setFCV command and let it complete successfully.");
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
        checkFCV(adminDB, lastStableFCV);

        jsTestLog("Verify that we are not allowed to prepare a transaction after downgrading.");
        session.startTransaction();
        assert.commandWorked(sessionDB[collName].insert({"b": 2}));
        assert.commandFailedWithCode(sessionDB.adminCommand({prepareTransaction: 1}),
                                     ErrorCodes.CommandNotSupported);
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
    } finally {
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "off"}));

        jsTestLog("Restore the original featureCompatibilityVersion.");
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);
    }

    session.endSession();
}());
