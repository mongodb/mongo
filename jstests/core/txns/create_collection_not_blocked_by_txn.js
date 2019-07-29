/**
 * Test that create collection only takes database IX lock and will not be blocked by transactions.
 *
 * @tags: [uses_transactions, requires_db_locking, assumes_unsharded_collection]
 */

(function() {
"use strict";

let dbName = 'create_collection_not_blocked_by_txn';
let mydb = db.getSiblingDB(dbName);

mydb.a.drop({writeConcern: {w: "majority"}});
mydb.b.drop({writeConcern: {w: "majority"}});
mydb.c.drop({writeConcern: {w: "majority"}});

assert.commandWorked(mydb.runCommand({insert: "a", documents: [{x: 1}]}));

const session = mydb.getMongo().startSession();
const sessionDb = session.getDatabase(dbName);

session.startTransaction();
// This holds a database IX lock and a collection IX lock on "a".
sessionDb.a.insert({y: 1});

// This only requires database IX lock.
assert.commandWorked(mydb.createCollection("b"));
// Implicit creation.
assert.commandWorked(mydb.runCommand({insert: "c", documents: [{x: 2}]}));

assert.commandWorked(session.commitTransaction_forTesting());
})();
