/**
 * Tests transactions that commit/abort after no writes.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "empty_commit_abort";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const doc = {_id: 1, a: 1, b: 1};
    assert.commandWorked(testColl.insert(doc));

    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    // ---- Test 1. No operations before commit ----
    session.startTransaction();
    assert.commandFailedWithCode(sessionDB.adminCommand({commitTransaction: 1}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // ---- Test 2. No operations before abort ----
    session.startTransaction();
    assert.commandFailedWithCode(sessionDB.adminCommand({abortTransaction: 1}),
                                 ErrorCodes.OperationNotSupportedInTransaction);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // ---- Test 3. Only reads before commit ----
    session.startTransaction();
    assert.eq(doc, sessionColl.findOne({a: 1}));
    assert.commandWorked(session.commitTransaction_forTesting());

    // ---- Test 4. Only reads before abort ----
    session.startTransaction();
    assert.eq(doc, sessionColl.findOne({a: 1}));
    assert.commandWorked(session.abortTransaction_forTesting());

    // ---- Test 5. Noop writes before commit ----
    session.startTransaction();
    let res = assert.commandWorked(sessionColl.update({_id: 1}, {$set: {b: 1}}));
    assert.eq(res.nMatched, 1, tojson(res));
    assert.eq(res.nModified, 0, tojson(res));
    assert.eq(res.nUpserted, 0, tojson(res));
    assert.commandWorked(session.commitTransaction_forTesting());

    // ---- Test 6. Noop writes before abort ----
    session.startTransaction();
    res = assert.commandWorked(sessionColl.update({_id: 1}, {$set: {b: 1}}));
    assert.eq(res.nMatched, 1, tojson(res));
    assert.eq(res.nModified, 0, tojson(res));
    assert.eq(res.nUpserted, 0, tojson(res));
    assert.commandWorked(session.abortTransaction_forTesting());

}());
