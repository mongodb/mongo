/**
 * Test that drop collection only takes database IX lock and will not be blocked by transactions.
 *
 * @tags: [uses_transactions, requires_db_locking, assumes_unsharded_collection]
 */

(function() {
    "use strict";

    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    let db = rst.getPrimary().getDB("test");

    assert.commandWorked(db.runCommand({insert: "a", documents: [{x: 1}]}));
    assert.commandWorked(db.runCommand({insert: "b", documents: [{x: 1}]}));

    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase("test");

    session.startTransaction();
    // This holds a database IX lock and a collection IX lock on "a".
    sessionDb.a.insert({y: 1});

    // This only requires database IX lock.
    assert.commandWorked(db.runCommand({drop: "b"}));

    assert.commandWorked(session.commitTransaction_forTesting());

    rst.stopSet();
})();
