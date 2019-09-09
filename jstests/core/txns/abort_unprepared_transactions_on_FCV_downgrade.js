/**
 * Test that open unprepared transactions are aborted on FCV downgrade. This test covers the
 * behavior between FCV downgrade and unprepared transactions as of v4.2. It is safe to change this
 * test's behavior or remove this test entirely if the behavior changes post v4.2.
 * @tags: [uses_transactions]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "abort_unprepared_transactions_on_FCV_downgrade";
const testDB = db.getSiblingDB(dbName);
const adminDB = db.getSiblingDB("admin");
testDB[collName].drop({writeConcern: {w: "majority"}});

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDB = session.getDatabase(dbName);

try {
    jsTestLog("Start a transaction.");
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandWorked(sessionDB[collName].insert({_id: "insert-1"}));

    jsTestLog("Attempt to drop the collection. This should fail due to the open transaction.");
    assert.commandFailedWithCode(testDB.runCommand({drop: collName, maxTimeMS: 1000}),
                                 ErrorCodes.MaxTimeMSExpired);

    jsTestLog("Downgrade the featureCompatibilityVersion.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(adminDB, lastStableFCV);

    jsTestLog("Drop the collection. This should succeed, since the transaction was aborted.");
    assert.commandWorked(testDB.runCommand({drop: collName}));

    jsTestLog("Test that committing the transaction fails, since it was aborted.");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
} finally {
    jsTestLog("Restore the original featureCompatibilityVersion.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);
}

session.endSession();
}());
