/**
 * Test that rename collection only takes database IX lock and will not be blocked by transactions.
 *
 * @tags: [uses_transactions, requires_db_locking, assumes_unsharded_collection]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let dbName = "rename_collection_not_blocked_by_txn";
let otherDbName = dbName + "_other";
let mydb = db.getSiblingDB(dbName);
const otherDb = db.getSiblingDB(otherDbName);

mydb.t.drop({writeConcern: {w: "majority"}});
mydb.a.drop({writeConcern: {w: "majority"}});
mydb.b.drop({writeConcern: {w: "majority"}});
mydb.c.drop({writeConcern: {w: "majority"}});
otherDb.d.drop({writeConcern: {w: "majority"}});
mydb.e.drop({writeConcern: {w: "majority"}});

assert.commandWorked(mydb.runCommand({insert: "t", documents: [{x: 1}]}));
assert.commandWorked(mydb.runCommand({insert: "a", documents: [{x: 1}]}));
assert.commandWorked(mydb.runCommand({insert: "b", documents: [{x: 1}]}));

const session = mydb.getMongo().startSession();
const sessionDb = session.getDatabase(dbName);

session.startTransaction();
// This holds a database IX lock and a collection IX lock on "test.t".
sessionDb.t.insert({y: 1});

// This only requires database IX lock.
assert.commandWorked(mydb.adminCommand({renameCollection: dbName + ".a", to: dbName + ".b", dropTarget: true}));
assert.commandWorked(mydb.adminCommand({renameCollection: dbName + ".b", to: dbName + ".c"}));
try {
    assert.commandWorked(mydb.adminCommand({renameCollection: dbName + ".c", to: otherDbName + ".d"}));
    assert.commandWorked(mydb.adminCommand({renameCollection: otherDbName + ".d", to: dbName + ".e"}));
} catch (e) {
    if (e.code == ErrorCodes.CommandFailed && FixtureHelpers.isMongos(mydb)) {
        // Rename across databases fails if the source and target DBs have different primary shards.
        assert(mydb.getDatabasePrimaryShardId() != otherDb.getDatabasePrimaryShardId());
    } else {
        throw e;
    }
}

assert.commandWorked(session.commitTransaction_forTesting());
