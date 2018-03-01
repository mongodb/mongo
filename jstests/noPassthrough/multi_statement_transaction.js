// Test basic transaction commits with two inserts.
// @tags: [requires_replication]
(function() {
    "use strict";
    load('jstests/libs/uuid_util.js');

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const testDB = rst.getPrimary().getDB(dbName);
    const coll = testDB.coll;

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

    // Insert a doc within the transaction.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-1"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
        // Only the first write in a transaction has autocommit flag.
        autocommit: false
    }));

    // Cannot read with default read concern.
    assert.eq(null, testDB.coll.findOne({_id: "insert-1"}));
    // But read in the same transaction returns the doc.
    let res = sessionDb.runCommand({
        find: collName,
        filter: {_id: "insert-1"},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber)
    });
    assert.commandWorked(res);
    assert.docEq([{_id: "insert-1"}], res.cursor.firstBatch);

    // Insert a doc within a transaction.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "insert-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber),
    }));

    // Cannot read with default read concern.
    assert.eq(null, testDB.coll.findOne({_id: "insert-1"}));
    // Cannot read with default read concern.
    assert.eq(null, testDB.coll.findOne({_id: "insert-2"}));

    assert.commandWorked(sessionDb.runCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(txnNumber),
    }));

    // Read with default read concern sees the committed transaction.
    assert.eq({_id: "insert-1"}, testDB.coll.findOne({_id: "insert-1"}));
    assert.eq({_id: "insert-2"}, testDB.coll.findOne({_id: "insert-2"}));

    // Oplog has the "applyOps" entry that includes two insert ops.
    const insertOps = [
        {op: 'i', ns: coll.getFullName(), o: {_id: "insert-1"}},
        {op: 'i', ns: coll.getFullName(), o: {_id: "insert-2"}},
    ];
    let topOfOplog = oplog.find().sort({$natural: -1}).limit(1).next();
    assert.eq(topOfOplog.txnNumber, NumberLong(txnNumber));
    assert.docEq(topOfOplog.o.applyOps, insertOps.map(x => Object.assign(x, {ui: uuid})));

    rst.stopSet();
}());
