// Test transactions including find-and-modify
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "find_and_modify_in_transaction";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    assert.commandWorked(
        testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));
    let txnNumber = 0;

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);

    jsTest.log("Prepopulate the collection.");
    assert.writeOK(testColl.insert([{_id: 0, a: 0}, {_id: 1, a: 1}, {_id: 2, a: 2}],
                                   {writeConcern: {w: "majority"}}));

    jsTest.log("Do a non-matching find-and-modify with remove.");
    let res = assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {a: 99},
        remove: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(null, res.value);

    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [{_id: 0, a: 0}, {_id: 1, a: 1}, {_id: 2, a: 2}]);

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a non-matching find-and-modify with update.");
    res = assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {a: 99},
        update: {$inc: {a: 100}},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(null, res.value);

    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [{_id: 0, a: 0}, {_id: 1, a: 1}, {_id: 2, a: 2}]);

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a matching find-and-modify with remove.");
    res = assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {a: 0},
        remove: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq({_id: 0, a: 0}, res.value);

    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [{_id: 1, a: 1}, {_id: 2, a: 2}]);

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a matching find-and-modify with update, requesting the old doc.");
    res = assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {a: 1},
        update: {$inc: {a: 100}},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq({_id: 1, a: 1}, res.value);

    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [{_id: 1, a: 101}, {_id: 2, a: 2}]);

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a matching find-and-modify with update, requesting the new doc.");
    res = assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {a: 2},
        update: {$inc: {a: 100}},
        new: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq({_id: 2, a: 102}, res.value);

    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [{_id: 1, a: 101}, {_id: 2, a: 102}]);

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a matching find-and-modify with upsert, requesting the new doc.");
    res = assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {_id: 2},
        update: {$inc: {a: 100}},
        upsert: true,
        new: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq({_id: 2, a: 202}, res.value);

    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [{_id: 1, a: 101}, {_id: 2, a: 202}]);

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a non-matching find-and-modify with upsert, requesting the old doc.");
    res = assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {a: 3},
        upsert: true,
        update: {$inc: {a: 100}},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(null, res.value);
    res = assert.commandWorked(sessionDb.runCommand({
        find: collName,
        filter: {a: 103},
        projection: {_id: 0},
        txnNumber: NumberLong(txnNumber),
        autocommit: false
    }));
    assert.docEq(res.cursor.firstBatch, [{a: 103}]);

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a non-matching find-and-modify with upsert, requesting the new doc.");
    res = assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {a: 4},
        update: {$inc: {a: 200}},
        upsert: true,
        new: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    const newdoc = res.value;
    assert.eq(204, newdoc.a);
    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {a: 204}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [newdoc]);

    // commitTransaction can only be run on the admin database.
    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));
}());
