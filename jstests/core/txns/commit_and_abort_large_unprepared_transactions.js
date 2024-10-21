/**
 * Tests support for unprepared transactions larger than 16MB.
 *
 * @tags: [uses_transactions]
 */
import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";

const dbName = "test";
const collName = "large_unprepared_transactions";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

// As we are not able to send a single request larger than 16MB, we insert two documents
// of 10MB each to create a "large" transaction.
const kSize10MB = 10 * 1024 * 1024;
function createLargeDocument(id) {
    return {_id: id, longString: new Array(kSize10MB).join("a")};
}

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

let doc1 = createLargeDocument(1);
let doc2 = createLargeDocument(2);
withRetryOnTransientTxnError(
    () => {
        // Test committing an unprepared large transaction with two 10MB inserts.
        try {
            session.startTransaction();
            assert.commandWorked(sessionColl.insert(doc1));
            assert.commandWorked(sessionColl.insert(doc2));
            assert.commandWorked(session.commitTransaction_forTesting());
        } catch (e) {
            switch (e.code) {
                case ErrorCodes.WriteConflict:
                    // It may be possible for this test to run in a passthrough where such a large
                    // transaction fills up the cache and cannot commit. The transaction will be
                    // rolled-back with a WriteConflict as a result.
                    jsTestLog("Ignoring WriteConflict due to large transaction's size");
                    break;
                case ErrorCodes.TemporarilyUnavailable:
                    // It may be possible that high cache pressure caused this operation to be
                    // rolled back. We should retry as this should be a transient error.
                    jsTestLog("Transaction was rolled back due to high cache pressure, retrying");
                    break;
                case ErrorCodes.TransactionTooLargeForCache:
                    // It may be possible that high cache pressure caused this operation to be
                    // rolled back because the transaction was too large for the cache.
                    // We should retry as this should be a transient error.
                    jsTestLog("Transaction was too large for cache, retrying");
                    break;
                default:
                    throw e;
            }
        }
    },
    () => {
        session.abortTransaction_forTesting();
    });
assert.sameMembers(sessionColl.find().toArray(), [doc1, doc2]);

// Test aborting an unprepared large transaction with two 10MB inserts.
let doc3 = createLargeDocument(3);
let doc4 = createLargeDocument(4);
withRetryOnTransientTxnError(
    () => {
        session.startTransaction();
        assert.commandWorked(sessionColl.insert(doc3));
        assert.commandWorked(sessionColl.insert(doc4));
        assert.commandWorked(session.abortTransaction_forTesting());
    },
    () => {
        session.abortTransaction_forTesting();
    });

assert.sameMembers(sessionColl.find({_id: {$gt: 2}}).toArray(), []);
