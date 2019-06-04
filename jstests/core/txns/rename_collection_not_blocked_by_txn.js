/**
 * Test that rename collection only takes database IX lock and will not be blocked by transactions.
 *
 * @tags: [uses_transactions, requires_db_locking, assumes_unsharded_collection]
 */

(function() {
    "use strict";

    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    let db = rst.getPrimary().getDB("test");

    assert.commandWorked(db.runCommand({insert: "t", documents: [{x: 1}]}));
    assert.commandWorked(db.runCommand({insert: "a", documents: [{x: 1}]}));
    assert.commandWorked(db.runCommand({insert: "b", documents: [{x: 1}]}));

    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase("test");

    session.startTransaction();
    // This holds a database IX lock and a collection IX lock on "test.t".
    sessionDb.t.insert({y: 1});

    // This only requires database IX lock.
    assert.commandWorked(
        db.adminCommand({renameCollection: "test.a", to: "test.b", dropTarget: true}));
    assert.commandWorked(db.adminCommand({renameCollection: "test.b", to: "test.c"}));

    assert.commandWorked(session.commitTransaction_forTesting());

    rst.stopSet();
})();
