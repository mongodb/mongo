// Test that prepared transactions don't block DDL operations on the non-conflicting collections.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession,
//   # prepareTransaction.
//   not_allowed_with_signed_security_token,
//   uses_transactions,
//   uses_prepare_transaction
// ]

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";

const dbName = jsTestName();
const collName = "transactions_collection";
const otherDBName = dbName + "_other";
const otherCollName = "transactions_collection_other";
const testDB = db.getSiblingDB(dbName);
const otherDB = db.getSiblingDB(otherDBName);

const session = testDB.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB[collName];

// Setup.
testDB.dropDatabase();
otherDB.dropDatabase();
assert.commandWorked(sessionColl.insert({_id: 1, x: 0}));

/**
 * Tests that DDL operations on non-conflicting namespaces don't block on transactions.
 */
function testSuccess(cmdDBName, ddlCmd) {
    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 1}, {$inc: {x: 1}}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(testDB.getSiblingDB(cmdDBName).runCommand(ddlCmd));
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
}

jsTest.log("Test 'create'.");
const createCmd = {
    create: collName,
};
testSuccess(otherDBName, createCmd);

jsTest.log("Test 'createIndexes'.");
const createIndexesCmd = {
    createIndexes: collName,
    indexes: [{key: {x: 1}, name: "x_1"}],
    writeConcern: {w: "majority"},
};
testSuccess(otherDBName, createIndexesCmd);

jsTest.log("Test 'dropIndexes'.");
const dropIndexesCmd = {
    dropIndexes: collName,
    index: "x_1",
};
testSuccess(otherDBName, dropIndexesCmd);

assert.commandWorked(
    sessionDB.runCommand({
        createIndexes: collName,
        indexes: [{name: "multiKeyField_1", key: {multiKeyField: 1}}],
        writeConcern: {w: "majority"},
    }),
);
jsTest.log("Test 'insert' that enables multi-key index on the same collection.");
const insertAndSetMultiKeyCmd = {
    insert: collName,
    documents: [{multiKeyField: [1, 2]}],
};
testSuccess(dbName, insertAndSetMultiKeyCmd);

jsTest.log("Test 'drop'.");
const dropCmd = {
    drop: collName,
};
testSuccess(otherDBName, dropCmd);

jsTest.log("Test 'renameCollection'.");
assert.commandWorked(otherDB.getCollection(collName).insert({x: "doc-for-rename-collection"}));
otherDB.runCommand({drop: otherCollName});
const renameCollectionCmd = {
    renameCollection: otherDBName + "." + collName,
    to: otherDBName + "." + otherCollName,
};
testSuccess("admin", renameCollectionCmd);

session.endSession();
