/**
 * Tests that we emit info level log messages for DDL write operations at
 * the expected points on both primaries and secondaries.
 * @tags: [
 *  uses_transactions,
 *  requires_fcv_70,
 * ]
 */
(function() {
"use strict";

const name = jsTestName();
const rst = ReplSetTest({name: name, nodes: 2});
rst.startSet();

const nodes = rst.nodeList();
rst.initiate({
    "_id": name,
    "members": [{"_id": 0, "host": nodes[0]}, {"_id": 1, "host": nodes[1], "priority": 0}]
});

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const testDB = primary.getDB(name);
const collectionName = "log_collection";

rst.awaitReplication();

assert.commandWorked(testDB.createCollection(collectionName));
assert.commandWorked(testDB.getCollection(collectionName).createIndex({x: 1}));
assert.commandWorked(
    testDB.runCommand({collMod: collectionName, index: {keyPattern: {x: 1}, hidden: true}}));
assert.commandWorked(testDB.getCollection(collectionName).dropIndex({x: 1}));

const newCollectionName = "log_collection_renamed";

assert.commandWorked(primary.getDB("admin").runCommand(
    {renameCollection: `${name}.${collectionName}`, to: `${name}.${newCollectionName}`}));
assert.commandWorked(testDB.runCommand({drop: newCollectionName}));
assert.commandWorked(testDB.runCommand({dropDatabase: 1}));

const txnCollectionName = "log_collection_txn";

const session = testDB.getMongo().startSession();
const sessionDB = session.getDatabase(name);

session.startTransaction();
assert.commandWorked(sessionDB.createCollection(txnCollectionName));
assert.commandWorked(sessionDB.getCollection(txnCollectionName).createIndex({x: 1}));
session.commitTransaction();

rst.awaitReplication();

const primaryLogCodes = [
    7360100,  // createIndexes in txn
    7360101,  // createIndexes
    7360102,  // create in txn
    7360103,  // create
    7360104,  // collMod
    7360105,  // dropDatabase
    7360106,  // drop
    7360107,  // dropIndexes
    7360108,  // renameCollection
];
for (const code of primaryLogCodes) {
    checkLog.contains(primary, code, 1 * 1000);
}

const opsLogged = [
    "createIndexes",
    "create",
    "collMod",
    "dropDatabase",
    "drop",
    "dropIndexes",
    "renameCollection",
];

for (const op of opsLogged) {
    checkLog.contains(secondary, new RegExp(`7360109.*${op}`), 1 * 1000);  // OplogBatcher message
    checkLog.contains(secondary, new RegExp(`7360110.*${op}`), 1 * 1000);  // OplogApplier message
}

rst.stopSet();
})();
