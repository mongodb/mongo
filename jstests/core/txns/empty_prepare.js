/**
 * Tests transactions that are prepared after no writes.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "empty_prepare";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop();
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const doc = {_id: 1, a: 1, b: 1};
    assert.commandWorked(testColl.insert(doc));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    // ---- Test 1. No operations before prepare ----

    session.startTransaction();
    assert.commandFailedWithCode(sessionDB.adminCommand({prepareTransaction: 1}),
                                 ErrorCodes.InvalidOptions);
    session.abortTransaction();

    // ---- Test 2. Only reads before prepare ----

    session.startTransaction();
    assert.eq(doc, sessionColl.findOne({a: 1}));
    assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));
    session.abortTransaction();

    // ---- Test 3. Noop writes before prepare ----

    session.startTransaction();
    let res = assert.commandWorked(sessionColl.update({a: 1}, {$set: {b: 1}}));
    assert.eq(res.nMatched, 1, tojson(res));
    assert.eq(res.nModified, 0, tojson(res));
    assert.eq(res.nUpserted, 0, tojson(res));
    assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));
    session.abortTransaction();

}());
