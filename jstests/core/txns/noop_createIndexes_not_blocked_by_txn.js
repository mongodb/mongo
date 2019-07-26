// Tests that no-op createIndex commands do not block behind transactions.
// @tags: [uses_transactions]
(function() {
"use strict";

const dbName = 'noop_createIndexes_not_blocked';
const collName = 'test';
const testDB = db.getSiblingDB(dbName);

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);

const isMongos = assert.commandWorked(db.runCommand("ismaster")).msg === "isdbgrid";
if (isMongos) {
    // Access the collection before creating indexes so it can be implicitly sharded.
    assert.eq(sessionDB[collName].find().itcount(), 0);
}

const createIndexesCommand = {
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}]
};
assert.commandWorked(sessionDB.runCommand(createIndexesCommand));

session.startTransaction();
assert.commandWorked(sessionDB[collName].insert({a: 5, b: 6}));

// This should not block because an identical index exists.
let res = testDB.runCommand(createIndexesCommand);
assert.commandWorked(res);
assert.eq(res.numIndexesBefore, res.numIndexesAfter);

// This should not block but return an error because the index exists with different options.
res = testDB.runCommand({
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "unique_a_1", unique: true}],
});
assert.commandFailedWithCode(res, ErrorCodes.IndexOptionsConflict);

// This should block and time out because the index does not already exist.
res = testDB.runCommand(
    {createIndexes: collName, indexes: [{key: {b: 1}, name: "b_1"}], maxTimeMS: 500});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

// This should block and time out because one of the indexes does not already exist.
res = testDB.runCommand({
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}, {key: {b: 1}, name: "b_1"}],
    maxTimeMS: 500
});
assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);

assert.commandWorked(session.commitTransaction_forTesting());
}());
