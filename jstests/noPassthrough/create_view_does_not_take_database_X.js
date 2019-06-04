/**
 * Test that create view only takes database IX lock.
 *
 * @tags: [uses_transactions, requires_db_locking]
 */

(function() {
    "use strict";

    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    let db = rst.getPrimary().getDB("test");

    assert.commandWorked(db.runCommand({insert: "a", documents: [{x: 1}]}));

    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase("test");

    session.startTransaction();
    // This holds a database IX lock and a collection IX lock on "a".
    sessionDb.a.insert({y: 1});

    // This only requires database IX lock.
    assert.commandWorked(db.createView("view", "a", []));

    assert.eq(db.view.find().toArray().length, 1);

    assert.commandWorked(session.commitTransaction_forTesting());

    rst.stopSet();
})();
