// Tests that it is illegal to implicitly create a collection using insert or upsert in a
// multi-document transaction.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "no_implicit_collection_creation_in_txn";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    jsTest.log("Cannot implicitly create a collection in a transaction using insert.");

    // Insert succeeds when the collection exists.
    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

    session.startTransaction({writeConcern: {w: "majority"}});
    sessionColl.insert({_id: "doc"});
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq({_id: "doc"}, testColl.findOne({_id: "doc"}));

    // Insert fails when the collection does not exist.
    assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));

    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandFailedWithCode(sessionColl.insert({_id: "doc"}),
                                 ErrorCodes.OperationNotSupportedInTransaction);

    // Committing the transaction should fail, since it should never have been started.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    assert.eq(null, testColl.findOne({_id: "doc"}));

    jsTest.log("Cannot implicitly create a collection in a transaction using update.");

    // Update with upsert=true succeeds when the collection exists.
    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

    session.startTransaction({writeConcern: {w: "majority"}});
    sessionColl.update({_id: "doc"}, {$set: {updated: true}}, {upsert: true});
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq({_id: "doc", updated: true}, testColl.findOne({_id: "doc"}));

    // Update with upsert=true fails when the collection does not exist.
    assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));

    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandFailedWithCode(
        sessionColl.update({_id: "doc"}, {$set: {updated: true}}, {upsert: true}),
        ErrorCodes.OperationNotSupportedInTransaction);

    // Committing the transaction should fail, since it should never have been started.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    assert.eq(null, testColl.findOne({_id: "doc"}));

    // Update with upsert=false succeeds when the collection does not exist.
    session.startTransaction({writeConcern: {w: "majority"}});
    assert.commandWorked(
        sessionColl.update({_id: "doc"}, {$set: {updated: true}}, {upsert: false}));
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq(null, testColl.findOne({_id: "doc"}));

    jsTest.log("Cannot implicitly create a collection in a transaction using findAndModify.");

    // findAndModify with upsert=true succeeds when the collection exists.
    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

    session.startTransaction({writeConcern: {w: "majority"}});
    let res = sessionColl.findAndModify(
        {query: {_id: "doc"}, update: {$set: {updated: true}}, upsert: true});
    assert.eq(null, res);
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq({_id: "doc", updated: true}, testColl.findOne({_id: "doc"}));

    // findAndModify with upsert=true fails when the collection does not exist.
    assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));

    session.startTransaction({writeConcern: {w: "majority"}});
    res = assert.throws(() => sessionColl.findAndModify(
                            {query: {_id: "doc"}, update: {$set: {updated: true}}, upsert: true}));
    assert.commandFailedWithCode(res, ErrorCodes.OperationNotSupportedInTransaction);

    // Committing the transaction should fail, since it should never have been started.
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    assert.eq(null, testColl.findOne({_id: "doc"}));

    // findAndModify with upsert=false succeeds when the collection does not exist.
    session.startTransaction({writeConcern: {w: "majority"}});
    res = sessionColl.findAndModify(
        {query: {_id: "doc"}, update: {$set: {updated: true}}, upsert: false});
    assert.eq(null, res);
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq(null, testColl.findOne({_id: "doc"}));

    session.endSession();
}());
