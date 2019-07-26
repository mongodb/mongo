/**
 * Tests that the object returned by currentOp() for an inactive transaction includes information
 * about the last client that has run an operation against this transaction.
 *
 * @tags: [uses_transactions]
 */

(function() {
'use strict';

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const collName = 'currentop_last_client_info';
const dbName = 'test';
const testDB = rst.getPrimary().getDB(dbName);
const adminDB = rst.getPrimary().getDB('admin');
testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
assert.commandWorked(testDB[collName].insert({x: 1}, {writeConcern: {w: "majority"}}));

// Start a new Session.
const lsid = assert.commandWorked(testDB.runCommand({startSession: 1})).id;
const txnNumber = NumberLong(0);
assert.commandWorked(testDB.runCommand({
    find: collName,
    lsid: lsid,
    txnNumber: txnNumber,
    readConcern: {level: "snapshot"},
    startTransaction: true,
    autocommit: false
}));

const currentOpFilter = {
    active: false,
    'lsid.id': {$eq: lsid.id},
    'client': {$exists: true}
};

let currentOp = adminDB.aggregate([{$currentOp: {}}, {$match: currentOpFilter}]).toArray();
assert.eq(currentOp.length, 1);

let currentOpEntry = currentOp[0];
const connectionId = currentOpEntry.connectionId;
// Check that the currentOp object contains information about the last client that has run an
// operation and that its values align with our expectations.
assert.eq(currentOpEntry.appName, "MongoDB Shell");
assert.eq(currentOpEntry.clientMetadata.application.name, "MongoDB Shell");
assert.eq(currentOpEntry.clientMetadata.driver.name, "MongoDB Internal Client");

// Create a new Client and run another operation on the same session.
const otherClient = new Mongo(rst.getPrimary().host);
assert.commandWorked(otherClient.getDB(dbName).runCommand(
    {find: collName, lsid: lsid, txnNumber: txnNumber, autocommit: false}));

currentOp = adminDB.aggregate([{$currentOp: {}}, {$match: currentOpFilter}]).toArray();
currentOpEntry = currentOp[0];
// Check that the last client that has ran an operation against this session has a different
// connectionId than the previous client.
assert.neq(currentOpEntry.connectionId, connectionId);

assert.commandWorked(testDB.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: txnNumber,
    autocommit: false,
    writeConcern: {w: 'majority'}
}));

rst.stopSet();
})();
