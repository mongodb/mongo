// Tests that multikey updates made inside a transaction are visible to that transaction's reads.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = 'test';
    const collName = 'testReadOwnMultikeyWrites';
    // Use majority write concern to clear the drop-pending that can cause lock conflicts with
    // transactions.
    db.getSiblingDB(dbName).getCollection(collName).drop({writeConcern: {w: "majority"}});

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    assert.commandWorked(sessionDb.runCommand({create: collName}));

    assert.writeOK(sessionColl.insert({a: 1}));
    assert.commandWorked(sessionColl.createIndex({a: 1}));

    session.startTransaction();
    assert.writeOK(sessionColl.update({}, {$set: {a: [1, 2, 3]}}));
    assert.eq(1, sessionColl.find({}, {_id: 0, a: 1}).sort({a: 1}).itcount());
    session.commitTransaction();

    assert.eq(1,
              db.getSiblingDB(dbName)
                  .getCollection(collName)
                  .find({}, {_id: 0, a: 1})
                  .sort({a: 1})
                  .itcount());
})();
