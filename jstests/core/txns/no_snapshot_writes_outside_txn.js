/**
 * Verify that readConcern: snapshot is not permitted on writes outside transactions.
 *
 * @tags: [uses_transactions]
 */

(function() {
    "use strict";
    const dbName = "test";
    const collName = "no_snapshot_writes_outside_txn";
    const testDB = db.getSiblingDB(dbName);

    // Set up the test collection.
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

    assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    // Initiate the session.
    const sessionOptions = {causalConsistency: false};
    let session = db.getMongo().startSession(sessionOptions);
    let sessionDb = session.getDatabase(dbName);
    let txnNumber = 0;
    let stmtId = 0;

    function tryWrites({testDB, useSnapshotReadSyntax, message}) {
        jsTestLog("Verify that inserts cannot use readConcern snapshot " + message);
        let cmd = {
            insert: collName,
            documents: [{_id: 0}],
            readConcern: {level: "snapshot"},
        };
        if (useSnapshotReadSyntax) {
            Object.assign(cmd, {txnNumber: NumberLong(txnNumber++), stmtId: NumberInt(stmtId)});
        }
        assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);

        jsTestLog("Verify that updates cannot use readConcern snapshot " + message);
        cmd = {
            update: collName,
            updates: [{q: {_id: 0}, u: {$set: {x: 1}}}],
            readConcern: {level: "snapshot"},
        };
        if (useSnapshotReadSyntax) {
            Object.assign(cmd, {txnNumber: NumberLong(txnNumber++), stmtId: NumberInt(stmtId)});
        }
        assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);

        jsTestLog("Verify that deletes cannot use readConcern snapshot " + message);
        cmd = {
            delete: collName,
            deletes: [{q: {_id: 0}, limit: 1}],
            readConcern: {level: "snapshot"},
        };
        if (useSnapshotReadSyntax) {
            Object.assign(cmd, {txnNumber: NumberLong(txnNumber++), stmtId: NumberInt(stmtId)});
        }
        assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);

        jsTestLog("Verify that findAndModify cannot use readConcern snapshot " + message);
        cmd = {
            findAndModify: collName,
            query: {_id: 0},
            remove: true,
            readConcern: {level: "snapshot"},
        };
        if (useSnapshotReadSyntax) {
            Object.assign(cmd, {txnNumber: NumberLong(txnNumber++), stmtId: NumberInt(stmtId)});
        }
        assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);
    }
    tryWrites({
        testDB: sessionDb,
        useSnapshotReadSyntax: true,
        message: "in session using snapshot read syntax."
    });
    tryWrites({testDB: sessionDb, useSnapshotReadSyntax: false, message: "in session."});
    tryWrites({testDB: testDB, useSnapshotReadSyntax: false, message: "outside session."});

    session.endSession();
}());
