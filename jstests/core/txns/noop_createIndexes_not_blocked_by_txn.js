// Tests that no-op createIndex commands do not block behind transactions.
// @tags: [uses_transactions]
(function() {
"use strict";

// TODO(SERVER-39704): Remove the following load after SERVER-39704 is completed
// For withTxnAndAutoRetryOnMongos.
load('jstests/libs/auto_retry_transaction_in_sharding.js');

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

// Default read concern level to use for transactions. Snapshot read concern is not supported in
// sharded transactions when majority reads are disabled.
if (!TestData.hasOwnProperty('defaultTransactionReadConcernLevel')) {
    TestData.defaultTransactionReadConcernLevel =
        TestData.enableMajorityReadConcern !== false ? 'snapshot' : 'local';
}

// TODO(SERVER-39704): We use the withTxnAndAutoRetryOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// withTxnAndAutoRetryOnMongos function can be removed
withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionDB[collName].insert({a: 5, b: 6}));

    // This should not block because an identical index exists.
    let res = testDB.runCommand(createIndexesCommand);
    assert.commandWorked(res);
    assert.eq(res.numIndexesBefore, res.numIndexesAfter);

    // This should not block but return an error because the index exists with different options.
    res = testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: "sparse_a_1", sparse: true}],
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
});
}());
