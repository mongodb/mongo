/**
 * Tests that downgrading from FCV4.2 to FCV4.0 while a large partial transaction is in progress
 * will fail on commit. This tests the case where a race condition may lead to the running
 * transaction committing before it could be aborted due to downgrade.
 *
 * @tags: [uses_transactions]
 */

(function() {
"use strict";

const dbName = "test";
const collName = "downgrade_fcv_while_large_partial_txn_in_progress";
const testDB = db.getSiblingDB(dbName);

assert.commandWorked(db.adminCommand(
    {configureFailPoint: "hangBeforeAbortingRunningTransactionsOnFCVDowngrade", mode: "alwaysOn"}));

// As we are not able to send a single request larger than 16MB, we insert two documents
// of 10MB each to create a "large" transaction.
const kSize10MB = 10 * 1024 * 1024;
function createLargeDocument(id) {
    return {_id: id, longString: new Array(kSize10MB).join("a")};
}

testDB[collName].drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

let doc1 = createLargeDocument(1);
let doc2 = createLargeDocument(2);

jsTestLog("Start a transaction and insert documents with sizes that add up to more than 16MB.");
session.startTransaction();
assert.commandWorked(sessionColl.insert(doc1));
assert.commandWorked(sessionColl.insert(doc2));

let downgradeFCV = startParallelShell(function() {
    load("jstests/libs/feature_compatibility_version.js");

    const testDB = db.getSiblingDB("test");
    const adminDB = db.getSiblingDB("admin");
    try {
        jsTestLog("Downgrade to FCV4.0.");
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
        checkFCV(adminDB, lastStableFCV);
    } finally {
        jsTestLog("Restore back to FCV4.2.");
        assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);
    }
});

// Wait until the in-memory FCV state has been changed to 4.0.
assert.soon(function() {
    const adminDB = db.getSiblingDB("admin");
    let res = adminDB.runCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    return "4.0" === res.featureCompatibilityVersion.version;
}, "Failed to detect the FCV change to 4.0 from server status.");

jsTestLog("Attempt to commit the large transaction using the FCV4.0 oplog format.");
assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                             ErrorCodes.TransactionTooLarge);

assert.commandWorked(db.adminCommand(
    {configureFailPoint: "hangBeforeAbortingRunningTransactionsOnFCVDowngrade", mode: "off"}));
downgradeFCV();
}());
