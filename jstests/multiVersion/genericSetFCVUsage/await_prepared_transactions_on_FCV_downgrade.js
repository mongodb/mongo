/**
 * Test that we wait for prepared transactions to finish during FCV downgrade. This test covers the
 * locking behavior as of v4.2. It is safe to change this test's behavior or remove this test
 * entirely if the locking behavior changes post v4.2.
 * @tags: [uses_transactions, uses_prepare_transaction, multiversion_incompatible]
 */
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

function runTest(downgradeFCV) {
    const rst = new ReplSetTest({nodes: [{binVersion: "latest"}]});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const dbName = "test";
    const collName = "await_prepared_transactions_on_FCV_downgrade";
    const testDB = primary.getDB(dbName);
    const adminDB = primary.getDB("admin");

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
            testDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}),
            ErrorCodes.LockTimeout);

        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "off"}));

        jsTestLog("Commit the prepared transaction.");
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

        jsTestLog("Rerun the setFCV command and let it complete successfully.");
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));
        checkFCV(adminDB, downgradeFCV);

    } finally {
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "off"}));

        jsTestLog("Restore the original featureCompatibilityVersion.");
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);
    }

    session.endSession();
    rst.stopSet();
}

runTest(lastLTSFCV);
if (lastLTSFCV !== lastContinuousFCV) {
    runTest(lastContinuousFCV);
}
}());
