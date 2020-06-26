/**
 * Verify that FCV 4.2 disables collection and index creation.
 *
 * @tags: [uses_transactions,
 *         # Creating collections inside multi-document transactions is supported only in v4.4
 *         # onwards.
 *         requires_fcv_44]
 */

load("jstests/libs/create_collection_txn_helpers.js");

function runFailedCollCreate(command, explicitCreate) {
    const session = db.getMongo().startSession();
    const collName = jsTestName();

    let sessionDB = session.getDatabase("test");
    let sessionColl = sessionDB[collName];
    sessionColl.drop({writeConcern: {w: "majority"}});

    session.startTransaction({readConcern: {level: "local"}, writeConcern: {w: "majority"}});
    assertCollCreateFailedWithCode(sessionDB,
                                   collName,
                                   command,
                                   explicitCreate,
                                   ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
}

function runFailedIndexCreate(collectionShouldExist) {
    const session = db.getMongo().startSession();
    const collName = jsTestName();

    let sessionDB = session.getDatabase("test");
    let sessionColl = sessionDB[collName];
    sessionColl.drop({writeConcern: {w: "majority"}});
    if (collectionShouldExist) {
        sessionColl.insert({a: 1});
    }

    session.startTransaction({readConcern: {level: "local"}, writeConcern: {w: "majority"}});
    assert.commandFailedWithCode(sessionColl.createIndex({a: 1}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
}

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

runFailedCollCreate("insert", true /*explicitCreate*/);
runFailedCollCreate("insert", false /*explicitCreate*/);
runFailedCollCreate("update", true /*explicitCreate*/);
runFailedCollCreate("update", false /*explicitCreate*/);
runFailedCollCreate("findAndModify", true /*explicitCreate*/);
runFailedCollCreate("findAndModify", false /*explicitCreate*/);

runFailedIndexCreate(true /*collectionShouldExist*/);
runFailedIndexCreate(false /*collectionShouldExist*/);

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
