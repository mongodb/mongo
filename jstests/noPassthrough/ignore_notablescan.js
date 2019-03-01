// Test that 'notablescan' parameter does not affect queries internal namespaces.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: {notablescan: true}}});
    rst.startSet();
    rst.initiate();

    const configDB = rst.getPrimary().getDB("config");
    const session = rst.getPrimary().getDB(dbName).getMongo().startSession();
    const primaryDB = session.getDatabase(dbName);

    // Implicitly create the collection outside of the transaction.
    assert.writeOK(primaryDB.getCollection(collName).insert({x: 1}));

    // Run a transaction so the 'config.transactions' collection is implicitly created.
    session.startTransaction();
    assert.writeOK(primaryDB.getCollection(collName).insert({x: 2}));
    session.commitTransaction();

    // Run a predicate query that would fail if we did not ignore the 'notablescan' flag.
    assert.eq(configDB.transactions.find({any_nonexistent_field: {$exists: true}}).itcount(), 0);

    // Run the same query against the user created collection honoring the 'notablescan' flag.
    // This will cause the query to fail as there is no viable query plan. Unfortunately,
    // the reported query error code is the cryptic 'BadValue'.
    assert.commandFailedWithCode(
        primaryDB.runCommand({find: collName, filter: {any_nonexistent_field: {$exists: true}}}),
        ErrorCodes.BadValue);

    rst.stopSet();
}());
