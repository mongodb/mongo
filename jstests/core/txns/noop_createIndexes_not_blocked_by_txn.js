// Tests that no-op createIndex commands do not block behind transactions.
// @tags: [uses_transactions]
// TODO(SERVER-39704): Remove the following load after SERVER-39704 is completed
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const dbName = "noop_createIndexes_not_blocked";
const collName = "test";
const testDB = db.getSiblingDB(dbName);

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);

if (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) {
    // Access the collection before creating indexes so it can be implicitly sharded.
    assert.eq(sessionDB[collName].find().itcount(), 0);
}

const createIndexesCommand = {
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}],
    writeConcern: {w: "majority"},
};
assert.commandWorked(sessionDB.runCommand(createIndexesCommand));

// Default read concern level to use for transactions.
if (!TestData.hasOwnProperty("defaultTransactionReadConcernLevel")) {
    TestData.defaultTransactionReadConcernLevel = "snapshot";
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
        indexes: [{key: {a: 1}, name: "storageEngine", storageEngine: {wiredTiger: {}}}],
    });
    assert.commandFailedWithCode(res, ErrorCodes.IndexOptionsConflict);

    // This should block and time out because the index does not already exist.
    res = testDB.runCommand({createIndexes: collName, indexes: [{key: {b: 1}, name: "b_1"}], maxTimeMS: 500});
    assert(ErrorCodes.isExceededTimeLimitError(res.code));

    // This should block and time out because one of the indexes does not already exist.
    res = testDB.runCommand({
        createIndexes: collName,
        indexes: [
            {key: {a: 1}, name: "a_1"},
            {key: {b: 1}, name: "b_1"},
        ],
        maxTimeMS: 500,
    });
    assert(ErrorCodes.isExceededTimeLimitError(res.code));
});
