/**
 * Test that renaming a collection through $out takes database IX lock and will not be blocked by
 * transactions. This test is created to ensure that the scenario occuring in the case linked in
 * SERVER-72703, where a compact operation (which took an IX DB lock) blocked a rename with $out,
 * which required an exclusive DB lock. Now that $out should take an IX DB lock, renaming with
 * $out should not be blocked by operations taking an IX lock. Renaming with $out across
 * different databases still takes an X lock.
 *
 * @tags: [uses_transactions, requires_db_locking, assumes_unsharded_collection]
 */
import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";

let dbName = jsTestName();
let mydb = db.getSiblingDB(dbName);

// Drop the collections that we will be using in this test, both for the transaction and for the
// rename operation, and wait for majority confirmation.
mydb.txn.drop({writeConcern: {w: "majority"}});
mydb.a.drop({writeConcern: {w: "majority"}});
mydb.b.drop({writeConcern: {w: "majority"}});
mydb.c.drop({writeConcern: {w: "majority"}});

// Populate our collections.
assert.commandWorked(mydb.txn.insert({x: 1}));
assert.commandWorked(mydb.a.insert([{x: 1}]));
assert.commandWorked(mydb.c.insert({x: 1}));

// Begin a session for our transaction.
const session = mydb.getMongo().startSession();
const sessionDb = session.getDatabase(dbName);

withRetryOnTransientTxnError(
    () => {
        session.startTransaction();
        // This holds a database IX lock and a collection IX lock on "test.txn".
        assert.commandWorked(sessionDb.t.insert({y: 1}));

        // $out should now also only require an IX lock.
        // Test the scenario where we rename collection 'a' to collection 'b', which doesn't exist.
        assert.commandWorked(mydb.runCommand(
            {aggregate: "a", pipeline: [{$out: {db: dbName, coll: "b"}}], cursor: {}}));
        // Now test the scenario where we rename collection 'b' to collection 'c', which does exist.
        // This should drop collection 'c'.
        assert.commandWorked(mydb.runCommand(
            {aggregate: "b", pipeline: [{$out: {db: dbName, coll: "c"}}], cursor: {}}));

        // Now commit the transaction.
        assert.commandWorked(session.commitTransaction_forTesting());
    },
    () => {
        session.abortTransaction();
    });
