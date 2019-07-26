/**
 * Tests that transactions larger than 16MB can only be created in FCV 4.2.
 *
 * @tags: [uses_transactions]
 */
(function() {
"uses strict";
load("jstests/libs/feature_compatibility_version.js");
load("jstests/core/txns/libs/prepare_helpers.js");

const dbName = "test";
const collName = "large_transactions_require_fcv42";
const testDB = db.getSiblingDB(dbName);
const adminDB = db.getSiblingDB('admin');

testDB[collName].drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

// As we are not able to send a single request larger than 16MB, we insert two documents
// of 10MB each to create a "large" transaction.
const kSize10MB = 10 * 1024 * 1024;
function createLargeDocument(id) {
    return {_id: id, longString: "a".repeat(kSize10MB)};
}

try {
    jsTestLog("Test that creating a transaction larger than 16MB succeeds in FCV 4.2.");
    let doc1 = createLargeDocument(1);
    let doc2 = createLargeDocument(2);

    checkFCV(adminDB, latestFCV);
    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc1));
    assert.commandWorked(sessionColl.insert(doc2));
    assert.commandWorked(session.commitTransaction_forTesting());

    jsTestLog("Downgrade the featureCompatibilityVersion.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(adminDB, lastStableFCV);

    jsTestLog("Test that trying to create a transaction larger than 16MB fails in FCV 4.0.");
    let doc3 = createLargeDocument(3);
    let doc4 = createLargeDocument(4);

    session.startTransaction();
    assert.commandWorked(sessionColl.insert(doc3));
    assert.commandFailedWithCode(sessionColl.insert(doc4), ErrorCodes.TransactionTooLarge);
    // We have to call 'abortTransaction' here to clear the transaction state in the shell.
    // Otherwise, the later call to 'startTransaction' will fail with 'Transaction already in
    // progress'.
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
} finally {
    jsTestLog("Restore to FCV 4.2.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(adminDB, latestFCV);
}

jsTestLog("Test that creating a transaction larger than 16MB succeeds after upgrading to FCV 4.2.");
let doc5 = createLargeDocument(5);
let doc6 = createLargeDocument(6);

session.startTransaction();
assert.commandWorked(sessionColl.insert(doc5));
assert.commandWorked(sessionColl.insert(doc6));
assert.commandWorked(session.commitTransaction_forTesting());

session.endSession();
}());
