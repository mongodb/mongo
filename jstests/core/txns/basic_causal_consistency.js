// Test that the shell helper supports causal consistency.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: endSession.
//   not_allowed_with_signed_security_token,
//   uses_transactions,
//   uses_snapshot_read_concern
// ]

// TODO (SERVER-39704): Remove the following load after SERVER-39704 is completed
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const dbName = "test";
const collName = "basic_causal_consistency";
const testDB = db.getSiblingDB(dbName);

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: true
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb.getCollection(collName);

// TODO (SERVER-39704): We use the withTxnAndAutoRetryOnMongos
// function to handle how MongoS will propagate a StaleShardVersion error as a
// TransientTransactionError. After SERVER-39704 is completed the
// withTxnAndAutoRetryOnMongos function can be removed
withTxnAndAutoRetryOnMongos(session, () => {
    // Performing a read first should work when snapshot readConcern is specified.
    assert.docEq(null, sessionColl.findOne({_id: "insert-1"}));

    assert.commandWorked(sessionColl.insert({_id: "insert-1"}));

    assert.docEq(null, sessionColl.findOne({_id: "insert-2"}));

    assert.docEq({_id: "insert-1"}, sessionColl.findOne({_id: "insert-1"}));
});

session.endSession();
