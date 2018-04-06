// Test transactions including multi-updates.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "multi_update_in_transaction";
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
    assert.writeOK(testColl.insert([{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1}],
                                   {writeConcern: {w: "majority"}}));

    jsTest.log("Do an empty multi-update.");
    let res = assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [{q: {a: 99}, u: {$set: {b: 1}}, multi: true}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(0, res.n);

    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1}]);

    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        // TODO(russotto): Majority write concern on commit is to avoid a WriteConflictError
        // writing to the transaction table.
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a single-result multi-update.");
    res = assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [{q: {a: 1}, u: {$set: {b: 1}}, multi: true}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(1, res.n);
    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [{_id: 0, a: 0}, {_id: 1, a: 0}, {_id: 2, a: 1, b: 1}]);

    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a multiple-result multi-update.");
    res = assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [{q: {a: 0}, u: {$set: {b: 2}}, multi: true}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(2, res.n);
    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch,
                 [{_id: 0, a: 0, b: 2}, {_id: 1, a: 0, b: 2}, {_id: 2, a: 1, b: 1}]);

    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a multiple-query multi-update.");
    res = assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [
            {q: {a: 0}, u: {$set: {c: 1}}, multi: true},
            {q: {_id: 2}, u: {$set: {c: 2}}, multi: true}
        ],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(3, res.n);
    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(
        res.cursor.firstBatch,
        [{_id: 0, a: 0, b: 2, c: 1}, {_id: 1, a: 0, b: 2, c: 1}, {_id: 2, a: 1, b: 1, c: 2}]);

    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));

    jsTest.log("Do a multi-update with upsert.");
    res = assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [{q: {_id: 3}, u: {$set: {d: 1}}, multi: true, upsert: true}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false
    }));
    assert.eq(1, res.n);
    assert.eq(res.upserted[0]._id, 3);
    res = assert.commandWorked(sessionDb.runCommand(
        {find: collName, filter: {}, txnNumber: NumberLong(txnNumber), autocommit: false}));
    assert.docEq(res.cursor.firstBatch, [
        {_id: 0, a: 0, b: 2, c: 1},
        {_id: 1, a: 0, b: 2, c: 1},
        {_id: 2, a: 1, b: 1, c: 2},
        {_id: 3, d: 1}
    ]);

    assert.commandWorked(sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber++),
        writeConcern: {w: "majority"},
        autocommit: false
    }));
}());
