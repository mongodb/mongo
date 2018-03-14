// Test transaction starting with read.
// @tags: [requires_replication]
(function() {
    "use strict";
    load('jstests/libs/uuid_util.js');

    const dbName = "test";
    const collName = "start_transaction_with_read";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const testDB = rst.getPrimary().getDB(dbName);
    const coll = testDB[collName];

    if (!testDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    testDB.runCommand({create: coll.getName(), writeConcern: {w: "majority"}});
    const uuid = getUUIDFromListCollections(testDB, coll.getName());
    const oplog = testDB.getSiblingDB('local').oplog.rs;
    let txnNumber = 0;

    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    // Non-transactional write to give something to find.
    const initialDoc = {_id: "pretransaction1", x: 0};
    assert.writeOK(sessionColl.insert(initialDoc, {writeConcern: {w: "majority"}}));

    jsTest.log("Start a transaction with a read");
    let res = assert.commandWorked(sessionDb.runCommand({
        find: collName,
        batchSize: 10,
        txnNumber: NumberLong(txnNumber),
        readConcern: {level: "snapshot"},
        // Only the first operation in a transaction has autocommit flag.
        autocommit: false
    }));
    assert.eq(res.cursor.firstBatch, [initialDoc]);

    jsTest.log("Insert two documents in a transaction");
    // Insert a doc within the transaction.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1"}],
        txnNumber: NumberLong(txnNumber),
    }));

    // Read in the same transaction returns the doc.
    res = sessionDb.runCommand(
        {find: collName, filter: {_id: "insert-1"}, txnNumber: NumberLong(txnNumber)});
    assert.commandWorked(res);
    assert.docEq([{_id: "insert-1"}], res.cursor.firstBatch);

    // Insert a doc within a transaction.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-2"}],
        txnNumber: NumberLong(txnNumber),
    }));

    assert.commandWorked(sessionDb.runCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
    }));

    // Read with default read concern sees the committed transaction.
    assert.eq({_id: "insert-1"}, coll.findOne({_id: "insert-1"}));
    assert.eq({_id: "insert-2"}, coll.findOne({_id: "insert-2"}));
    assert.eq(initialDoc, coll.findOne(initialDoc));

    // Oplog has the "applyOps" entry that includes two insert ops.
    const insertOps = [
        {op: 'i', ns: coll.getFullName(), o: {_id: "insert-1"}},
        {op: 'i', ns: coll.getFullName(), o: {_id: "insert-2"}},
    ];
    let topOfOplog = oplog.find().sort({$natural: -1}).limit(1).next();
    assert.eq(topOfOplog.txnNumber, NumberLong(txnNumber));
    assert.docEq(topOfOplog.o.applyOps, insertOps.map(x => Object.assign(x, {ui: uuid})));

    session.endSession();
    rst.stopSet();
}());
