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

    let txnNumber = 0;

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    jsTest.log("Cannot implicitly create a collection in a transaction using insert.");

    // Insert succeeds when the collection exists.
    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "doc"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    // commitTransaction can only be called on the admin database.
    assert.commandWorked(sessionDb.adminCommand(
        {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.eq({_id: "doc"}, testColl.findOne({_id: "doc"}));

    // Insert fails when the collection does not exist.
    assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "doc"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.NamespaceNotFound);
    // commitTransaction can only be called on the admin database.
    assert.commandFailedWithCode(
        sessionDb.adminCommand(
            {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}),
        ErrorCodes.NoSuchTransaction);
    assert.eq(null, testColl.findOne({_id: "doc"}));

    jsTest.log("Cannot implicitly create a collection in a transaction using update.");

    // Update with upsert=true succeeds when the collection exists.
    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [{q: {_id: "doc"}, u: {$set: {updated: true}}, upsert: true}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    // commitTransaction can only be called on the admin database.
    assert.commandWorked(sessionDb.adminCommand(
        {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.eq({_id: "doc", updated: true}, testColl.findOne({_id: "doc"}));

    // Update with upsert=true fails when the collection does not exist.
    assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));
    assert.commandFailedWithCode(sessionDb.runCommand({
        update: collName,
        updates: [{q: {_id: "doc"}, u: {$set: {updated: true}}, upsert: true}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.NamespaceNotFound);
    // commitTransaction can only be called on the admin database.
    assert.commandFailedWithCode(
        sessionDb.adminCommand(
            {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}),
        ErrorCodes.NoSuchTransaction);
    assert.eq(null, testColl.findOne({_id: "doc"}));

    // Update without upsert=true succeeds when the collection does not exist.
    assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [{q: {_id: "doc"}, u: {$set: {updated: true}}}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    // commitTransaction can only be called on the admin database.
    assert.commandWorked(sessionDb.adminCommand(
        {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.eq(null, testColl.findOne({_id: "doc"}));

    jsTest.log("Cannot implicitly create a collection in a transaction using findAndModify.");

    // findAndModify with upsert=true succeeds when the collection exists.
    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {_id: "doc"},
        update: {$set: {updated: true}},
        upsert: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    // commitTransaction can only be called on the admin database.
    assert.commandWorked(sessionDb.adminCommand(
        {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.eq({_id: "doc", updated: true}, testColl.findOne({_id: "doc"}));

    // findAndModify with upsert=true fails when the collection does not exist.
    assert.commandWorked(testDB.runCommand({drop: collName, writeConcern: {w: "majority"}}));
    assert.commandFailedWithCode(sessionDb.runCommand({
        findAndModify: collName,
        query: {_id: "doc"},
        update: {$set: {updated: true}},
        upsert: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    }),
                                 ErrorCodes.NamespaceNotFound);
    // commitTransaction can only be called on the admin database.
    assert.commandFailedWithCode(
        sessionDb.adminCommand(
            {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}),
        ErrorCodes.NoSuchTransaction);
    assert.eq(null, testColl.findOne({_id: "doc"}));

    // findAndModify without upsert=true succeeds when the collection does not exist.
    assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {_id: "doc"},
        update: {$set: {updated: true}},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(++txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    // commitTransaction can only be called on the admin database.
    assert.commandWorked(sessionDb.adminCommand(
        {commitTransaction: 1, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.eq(null, testColl.findOne({_id: "doc"}));

    session.endSession();
}());
