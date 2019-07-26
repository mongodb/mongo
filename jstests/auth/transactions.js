// Tests that users can only use transactions that they created.
// @tags: [uses_transactions]
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1"});
rst.startSet();
rst.initiate();

const adminDB = rst.getPrimary().getDB("admin");

// Create the admin user.
assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
assert.eq(1, adminDB.auth("admin", "admin"));

// Set up the test database.
const dbName = "test";
const collName = "transactions";
const testDB = adminDB.getSiblingDB(dbName);
testDB.dropDatabase();
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

// Create two users, "Alice" and "Mallory".
assert.commandWorked(testDB.runCommand({createUser: "Alice", pwd: "pwd", roles: ["readWrite"]}));
assert.commandWorked(testDB.runCommand({createUser: "Mallory", pwd: "pwd", roles: ["readWrite"]}));
adminDB.logout();

// Alice starts a transaction.
assert.eq(1, testDB.auth("Alice", "pwd"));
const lsid = assert.commandWorked(testDB.runCommand({startSession: 1})).id;
assert.commandWorked(testDB.runCommand({
    insert: collName,
    documents: [{_id: "alice-1"}],
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(0),
    startTransaction: true,
    autocommit: false
}));
testDB.logout();

// Mallory cannot continue the transaction. Using the same lsid for two different users creates
// two distinct sessions on the server. Mallory's session does not have an open transaction.
assert.eq(1, testDB.auth("Mallory", "pwd"));
assert.commandFailedWithCode(testDB.runCommand({
    insert: collName,
    documents: [{_id: "mallory"}],
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(1),
    autocommit: false
}),
                             ErrorCodes.NoSuchTransaction);

// Mallory cannot commit the transaction.
assert.commandFailedWithCode(adminDB.runCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(1),
    autocommit: false,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.NoSuchTransaction);

// Mallory cannot abort the transaction.
assert.commandFailedWithCode(adminDB.runCommand({
    abortTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(1),
    autocommit: false,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.NoSuchTransaction);
testDB.logout();

// An unauthenticated user cannot continue the transaction.
assert.commandFailedWithCode(testDB.runCommand({
    insert: collName,
    documents: [{_id: "unauthenticated"}],
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(1),
    autocommit: false
}),
                             ErrorCodes.Unauthorized);

// An unauthenticated user cannot commit the transaction.
assert.commandFailedWithCode(adminDB.runCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(1),
    autocommit: false,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.Unauthorized);

// An unauthenticated user cannot abort the transaction.
assert.commandFailedWithCode(adminDB.runCommand({
    abortTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(1),
    autocommit: false,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.Unauthorized);

// Alice can continue the transaction.
assert.eq(1, testDB.auth("Alice", "pwd"));
assert.commandWorked(testDB.runCommand({
    insert: collName,
    documents: [{_id: "alice-2"}],
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(1),
    autocommit: false
}));

// Alice can commit the transaction.
assert.commandWorked(adminDB.runCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(0),
    stmtId: NumberInt(2),
    autocommit: false,
    writeConcern: {w: "majority"}
}));

// We do not see the writes from Mallory or the unauthenticated user.
assert.eq(1, testDB[collName].find({_id: "alice-1"}).itcount());
assert.eq(1, testDB[collName].find({_id: "alice-2"}).itcount());
assert.eq(0, testDB[collName].find({_id: "mallory"}).itcount());
assert.eq(0, testDB[collName].find({_id: "unauthenticated"}).itcount());

assert.commandWorked(testDB.runCommand({endSessions: [lsid]}));
testDB.logout();
rst.stopSet();
}());
