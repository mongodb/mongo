/**
 * Test that rename collection only takes database IX lock and will not be blocked by transactions.
 *
 * @tags: [uses_transactions, requires_db_locking, assumes_unsharded_collection]
 */

(function() {
"use strict";

let dbName = 'rename_collection_not_blocked_by_txn';
let mydb = db.getSiblingDB(dbName);

mydb.t.drop({writeConcern: {w: "majority"}});
mydb.a.drop({writeConcern: {w: "majority"}});
mydb.b.drop({writeConcern: {w: "majority"}});
mydb.c.drop({writeConcern: {w: "majority"}});

assert.commandWorked(mydb.runCommand({insert: "t", documents: [{x: 1}]}));
assert.commandWorked(mydb.runCommand({insert: "a", documents: [{x: 1}]}));
assert.commandWorked(mydb.runCommand({insert: "b", documents: [{x: 1}]}));

const session = mydb.getMongo().startSession();
const sessionDb = session.getDatabase(dbName);

session.startTransaction();
// This holds a database IX lock and a collection IX lock on "test.t".
sessionDb.t.insert({y: 1});

// This only requires database IX lock.
assert.commandWorked(
    mydb.adminCommand({renameCollection: dbName + ".a", to: dbName + ".b", dropTarget: true}));
assert.commandWorked(mydb.adminCommand({renameCollection: dbName + ".b", to: dbName + ".c"}));

assert.commandWorked(session.commitTransaction_forTesting());
})();
