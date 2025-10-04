/*
 * This test ensures that dbstats does not conflict with multi-statement transactions as a result of
 * taking MODE_S locks that are incompatible with MODE_IX needed for writes.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: endSession.
 *   not_allowed_with_signed_security_token,
 *   uses_transactions
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let dbName = "dbstats_not_blocked_by_txn";
let mydb = db.getSiblingDB(dbName);

mydb.foo.drop({writeConcern: {w: "majority"}});
mydb.createCollection("foo", {writeConcern: {w: "majority"}});

let session = db.getMongo().startSession();
let sessionDb = session.getDatabase(dbName);

if (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) {
    // Before starting the transaction below, access the collection so it can be implicitly
    // sharded and force all shards to refresh their database versions because the refresh
    // requires an exclusive lock and would block behind the transaction.
    assert.eq(sessionDb.foo.find().itcount(), 0);
    assert.commandWorked(sessionDb.runCommand({listCollections: 1, nameOnly: true}));
}

session.startTransaction();
assert.commandWorked(sessionDb.foo.insert({x: 1}));

let res = mydb.runCommand({dbstats: 1, maxTimeMS: 10 * 1000});
assert.commandWorked(res, "dbstats should have succeeded and not timed out");

assert.commandWorked(session.commitTransaction_forTesting());
session.endSession();
