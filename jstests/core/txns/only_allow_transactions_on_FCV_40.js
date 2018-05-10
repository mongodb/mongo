// Tests that transactions only work on FCV 4.0.
// @tags: [uses_transactions]
(function() {
    "use strict";
    load("jstests/libs/feature_compatibility_version.js");

    const dbName = "test";
    const adminName = "admin";
    const collName = "only_allow_transactions_on_FCV_40";
    const testDB = db.getSiblingDB(dbName);
    const adminDB = db.getSiblingDB(adminName);
    const testColl = testDB[collName];

    testColl.drop();

    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    jsTestLog("Start transaction");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // Insert a document within the transaction.
    assert.commandWorked(sessionDb[collName].insert({_id: "insert-1", a: 0}));

    session.commitTransaction();

    try {
        // Set the FCV to 3.6, which should not be compatible with transactions.
        jsTestLog("Downgrade to FCV 3.6 and attempt to run a transaction.");
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastStableFCV}));
        checkFCV(adminDB, lastStableFCV);

        // The startTransaction helper does not actually send a request to the server.
        session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

        // This insert is the first statement of the transaction, and it should fail because we are
        // not in FCV 4.0.
        assert.commandFailedWithCode(sessionDb[collName].insert({_id: "insert-2", a: 0}), 50773);
    } finally {
        // Set the FCV to 4.0 again so that tests running after this one in core/txns pass.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);

        session.endSession();
    }

}());
