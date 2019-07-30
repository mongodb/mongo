// Test that open transactions block DDL operations on the involved collections.
// @tags: [uses_transactions]
(function() {
"use strict";

load("jstests/libs/parallelTester.js");  // for Thread.

const dbName = "transactions_block_ddl";
const collName = "transactions_block_ddl";
const otherDBName = "transactions_block_ddl_other";
const otherCollName = "transactions_block_ddl_other";
const testDB = db.getSiblingDB(dbName);

const session = testDB.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB[collName];

/**
 * Tests that DDL operations block on transactions and fail when their maxTimeMS expires.
 */
function testTimeout(cmdDBName, ddlCmd) {
    // Setup.
    sessionDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(sessionColl.createIndex({b: 1}, {name: "b_1"}));

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({a: 5, b: 6}));
    assert.commandFailedWithCode(
        testDB.getSiblingDB(cmdDBName).runCommand(Object.assign({}, ddlCmd, {maxTimeMS: 500})),
        ErrorCodes.MaxTimeMSExpired);
    assert.commandWorked(session.commitTransaction_forTesting());
}

/**
 * Tests that DDL operations block on transactions but can succeed once the transaction commits.
 */
function testSuccessOnTxnCommit(cmdDBName, ddlCmd, currentOpFilter) {
    // Setup.
    sessionDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(sessionColl.createIndex({b: 1}, {name: "b_1"}));

    jsTestLog("About to start tranasction");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({a: 5, b: 6}));
    jsTestLog("Transaction started, running ddl operation " + ddlCmd);
    let thread = new Thread(function(cmdDBName, ddlCmd) {
        return db.getSiblingDB(cmdDBName).runCommand(ddlCmd);
    }, cmdDBName, ddlCmd);
    thread.start();
    // Wait for the DDL operation to have pending locks.
    assert.soon(
        function() {
            // Note that we cannot use the $currentOp agg stage because it acquires locks
            // (SERVER-35289).
            return testDB.currentOp({$and: [currentOpFilter, {waitingForLock: true}]})
                       .inprog.length === 1;
        },
        function() {
            return "Failed to find DDL command in currentOp output: " +
                tojson(testDB.currentOp().inprog);
        });
    jsTestLog("Committing transaction");
    assert.commandWorked(session.commitTransaction_forTesting());
    jsTestLog("Transaction committed, waiting for ddl operation to complete.");
    thread.join();
    assert.commandWorked(thread.returnData());
}

jsTestLog("Testing that 'drop' blocks on transactions");
const dropCmd = {
    drop: collName,
    writeConcern: {w: "majority"}
};
testTimeout(dbName, dropCmd);
testSuccessOnTxnCommit(dbName, dropCmd, {"command.drop": collName});

jsTestLog("Testing that 'dropDatabase' blocks on transactions");
const dropDatabaseCmd = {
    dropDatabase: 1,
    writeConcern: {w: "majority"}
};
testTimeout(dbName, dropDatabaseCmd);
testSuccessOnTxnCommit(dbName, dropDatabaseCmd, {"command.dropDatabase": 1});

jsTestLog("Testing that 'renameCollection' within databases blocks on transactions");
testDB.runCommand({drop: otherCollName, writeConcern: {w: "majority"}});
const renameCollectionCmdSameDB = {
    renameCollection: sessionColl.getFullName(),
    to: dbName + "." + otherCollName,
    writeConcern: {w: "majority"}
};
testTimeout("admin", renameCollectionCmdSameDB);
testSuccessOnTxnCommit(
    "admin", renameCollectionCmdSameDB, {"command.renameCollection": sessionColl.getFullName()});

jsTestLog("Testing that 'renameCollection' across databases blocks on transactions");
testDB.getSiblingDB(otherDBName).runCommand({drop: otherCollName, writeConcern: {w: "majority"}});
const renameCollectionCmdDifferentDB = {
    renameCollection: sessionColl.getFullName(),
    to: otherDBName + "." + otherCollName,
    writeConcern: {w: "majority"}
};
testTimeout("admin", renameCollectionCmdDifferentDB);
testSuccessOnTxnCommit("admin",
                       renameCollectionCmdDifferentDB,
                       {"command.renameCollection": sessionColl.getFullName()});

jsTestLog("Testing that 'createIndexes' blocks on transactions");
// The transaction will insert a document that has a field 'a'.
const createIndexesCmd = {
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}],
    writeConcern: {w: "majority"}
};
testTimeout(dbName, createIndexesCmd);
testSuccessOnTxnCommit(dbName, createIndexesCmd, {"command.createIndexes": collName});

jsTestLog("Testing that 'dropIndexes' blocks on transactions");
// The setup creates an index on {b: 1} called 'b_1'. The transaction will insert a document
// that has a field 'b'.
const dropIndexesCmd = {
    dropIndexes: collName,
    index: "b_1",
    writeConcern: {w: "majority"}
};
testTimeout(dbName, dropIndexesCmd);
testSuccessOnTxnCommit(dbName, dropIndexesCmd, {"command.dropIndexes": collName});
session.endSession();
}());
