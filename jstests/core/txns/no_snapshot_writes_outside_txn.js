/**
 * Verify that readConcern: snapshot is not permitted for writes outside transactions.
 *
 * @tags: [uses_transactions]
 */

(function() {
"use strict";
const dbName = "test";
const collName = "no_read_concern_snapshot_outside_txn";
const testDB = db.getSiblingDB(dbName);

// Set up the test collection.
testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

// Initiate the session.
const sessionOptions = {
    causalConsistency: false
};
let session = db.getMongo().startSession(sessionOptions);
let sessionDb = session.getDatabase(dbName);
let txnNumber = 0;
let stmtId = 0;

function tryCommands({testDB, message}) {
    jsTestLog("Verify that inserts cannot use readConcern snapshot " + message);
    let cmd = {
        insert: collName,
        documents: [{_id: 0}],
        readConcern: {level: "snapshot"},
    };
    assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);

    jsTestLog("Verify that updates cannot use readConcern snapshot " + message);
    cmd = {
        update: collName,
        updates: [{q: {_id: 0}, u: {$set: {x: 1}}}],
        readConcern: {level: "snapshot"},
    };
    assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);

    jsTestLog("Verify that deletes cannot use readConcern snapshot " + message);
    cmd = {
        delete: collName,
        deletes: [{q: {_id: 0}, limit: 1}],
        readConcern: {level: "snapshot"},
    };
    assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);

    jsTestLog("Verify that findAndModify cannot use readConcern snapshot " + message);
    cmd = {
        findAndModify: collName,
        query: {_id: 0},
        remove: true,
        readConcern: {level: "snapshot"},
    };
    assert.commandFailedWithCode(testDB.runCommand(cmd), ErrorCodes.InvalidOptions);
}
tryCommands({testDB: sessionDb, message: "in session."});
tryCommands({testDB: testDB, message: "outside session."});

session.endSession();
}());
