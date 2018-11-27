/*
 * This test ensures that dbstats does not conflict with multi-statement transactions as a result of
 * taking MODE_S locks that are incompatible with MODE_IX needed for writes.
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";
    var dbName = 'dbstats_not_blocked_by_txn';
    var mydb = db.getSiblingDB(dbName);

    mydb.foo.drop({writeConcern: {w: "majority"}});
    mydb.createCollection("foo", {writeConcern: {w: "majority"}});

    var session = db.getMongo().startSession();
    var sessionDb = session.getDatabase(dbName);

    const isMongos = assert.commandWorked(db.runCommand("ismaster")).msg === "isdbgrid";
    if (isMongos) {
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
}());
