// Test basic multi-statement transaction.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession.
//   not_allowed_with_signed_security_token,
//   uses_transactions
// ]

// TODO(SERVER-39704): Remove the following load after SERVER-39704 is completed
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const dbName = "test";
const collName = "multi_statement_transaction";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false,
};
const session = db.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

/***********************************************************************************************
 * Insert two documents in a transaction.
 **********************************************************************************************/

assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));

jsTest.log("Insert two documents in a transaction");

// TODO(SERVER-39704): We use the withTxnAndAutoRetryOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// withTxnAndAutoRetryOnMongos function can be removed
withTxnAndAutoRetryOnMongos(session, () => {
    // Insert a doc within the transaction.
    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));

    // Cannot read with default read concern.
    assert.eq(null, testColl.findOne({_id: "insert-1"}));
    // But read in the same transaction returns the doc.
    assert.docEq({_id: "insert-1"}, sessionColl.findOne());

    // Read with aggregation also returns the document.
    let docs = sessionColl.aggregate([{$match: {_id: "insert-1"}}]).toArray();
    assert.sameMembers([{_id: "insert-1"}], docs);

    // Insert a doc within a transaction.
    assert.commandWorked(sessionColl.insert({_id: "insert-2"}));

    // Cannot read with default read concern.
    assert.eq(null, testColl.findOne({_id: "insert-1"}));
    // Cannot read with default read concern.
    assert.eq(null, testColl.findOne({_id: "insert-2"}));
});

// Read with default read concern sees the committed transaction.
assert.eq({_id: "insert-1"}, testColl.findOne({_id: "insert-1"}));
assert.eq({_id: "insert-2"}, testColl.findOne({_id: "insert-2"}));

/***********************************************************************************************
 * Update documents in a transaction.
 **********************************************************************************************/

assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));

jsTest.log("Update documents in a transaction");

// Insert the docs to be updated.
assert.commandWorked(
    sessionColl.insert(
        [
            {_id: "update-1", a: 0},
            {_id: "update-2", a: 0},
        ],
        {writeConcern: {w: "majority"}},
    ),
);

// Update the docs in a new transaction.

// TODO(SERVER-39704): We use the withTxnAndAutoRetryOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// withTxnAndAutoRetryOnMongos function can be removed
withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionColl.update({_id: "update-1"}, {$inc: {a: 1}}));

    // Batch update in transaction.
    let bulk = sessionColl.initializeUnorderedBulkOp();
    bulk.find({_id: "update-1"}).updateOne({$inc: {a: 1}});
    bulk.find({_id: "update-2"}).updateOne({$inc: {a: 1}});
    assert.commandWorked(bulk.execute());

    // Cannot read with default read concern.
    assert.eq({_id: "update-1", a: 0}, testColl.findOne({_id: "update-1"}));
    assert.eq({_id: "update-2", a: 0}, testColl.findOne({_id: "update-2"}));
});

// Read with default read concern sees the committed transaction.
assert.eq({_id: "update-1", a: 2}, testColl.findOne({_id: "update-1"}));
assert.eq({_id: "update-2", a: 1}, testColl.findOne({_id: "update-2"}));

/***********************************************************************************************
 * Insert, update and read documents in a transaction.
 **********************************************************************************************/

assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));

jsTest.log("Insert, update and read documents in a transaction");

// TODO(SERVER-39704): We use the withTxnAndAutoRetryOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// withTxnAndAutoRetryOnMongos function can be removed
withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionColl.insert([{_id: "doc-1"}, {_id: "doc-2"}]));

    // Update the two docs in transaction.
    assert.commandWorked(sessionColl.update({_id: "doc-1"}, {$inc: {a: 1}}));
    assert.commandWorked(sessionColl.update({_id: "doc-2"}, {$inc: {a: 1}}));

    // Cannot read with default read concern.
    assert.eq(null, testColl.findOne({_id: "doc-1"}));
    assert.eq(null, testColl.findOne({_id: "doc-2"}));

    // But read in the same transaction returns the docs.
    let docs = sessionColl.find({$or: [{_id: "doc-1"}, {_id: "doc-2"}]}).toArray();
    assert.sameMembers(
        [
            {_id: "doc-1", a: 1},
            {_id: "doc-2", a: 1},
        ],
        docs,
    );
});

// Read with default read concern sees the committed transaction.
assert.eq({_id: "doc-1", a: 1}, testColl.findOne({_id: "doc-1"}));
assert.eq({_id: "doc-2", a: 1}, testColl.findOne({_id: "doc-2"}));

jsTest.log("Insert and delete documents in a transaction");

assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));

assert.commandWorked(testColl.insert([{_id: "doc-1"}, {_id: "doc-2"}], {writeConcern: {w: "majority"}}));

// TODO(SERVER-39704): We use the withTxnAndAutoRetryOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// withTxnAndAutoRetryOnMongos function can be removed
withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionColl.insert({_id: "doc-3"}));

    // Remove three docs in transaction.
    assert.commandWorked(sessionColl.remove({_id: "doc-1"}));

    // Batch delete.
    let bulk = sessionColl.initializeUnorderedBulkOp();
    bulk.find({_id: "doc-2"}).removeOne();
    bulk.find({_id: "doc-3"}).removeOne();
    assert.commandWorked(bulk.execute());

    // Cannot read the new doc and still see the to-be removed docs with default read concern.
    assert.eq({_id: "doc-1"}, testColl.findOne({_id: "doc-1"}));
    assert.eq({_id: "doc-2"}, testColl.findOne({_id: "doc-2"}));
    assert.eq(null, testColl.findOne({_id: "doc-3"}));

    // But read in the same transaction sees the docs get deleted.
    let docs = sessionColl.find({$or: [{_id: "doc-1"}, {_id: "doc-2"}, {_id: "doc-3"}]}).toArray();
    assert.sameMembers([], docs);
});

// Read with default read concern sees the commmitted transaction.
assert.eq(null, testColl.findOne({_id: "doc-1"}));
assert.eq(null, testColl.findOne({_id: "doc-2"}));
assert.eq(null, testColl.findOne({_id: "doc-3"}));

// Transaction involving several collections
{
    assert.commandWorked(testColl.remove({}, {writeConcern: {w: "majority"}}));

    const collName2 = "multi_statement_transaction_2";
    const testColl2 = testDB[collName2];
    testColl2.drop();
    assert.commandWorked(testDB.createCollection(collName2));

    const sessionColl2 = sessionDb[collName2];
    withTxnAndAutoRetryOnMongos(session, () => {
        assert.commandWorked(sessionColl.insert({_id: "doc-1"}));
        assert.commandWorked(sessionColl2.insert({_id: "doc-2"}));
    });

    assert.eq(1, testColl.find().itcount());
    assert.eq(1, testColl2.find().itcount());
    assert.eq({_id: "doc-1"}, testColl.findOne({_id: "doc-1"}));
    assert.eq({_id: "doc-2"}, testColl2.findOne({_id: "doc-2"}));
}

session.endSession();
