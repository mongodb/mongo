/**
 * A transaction with default readConcern ("local") does not see writes from another session.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "default_read_concern";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB[collName];

    // Prepare the collection
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    jsTestLog("Start a transaction with default readConcern");
    session.startTransaction();

    // Inserts outside transaction aren't visible, even after they are
    // majority-committed. (It is not a requirement that transactions with local
    // readConcern do not see writes from another session. At some point, it
    // would be desirable to have a transaction with readConcern local or
    // majority see writes from other sessions. However, our current
    // implementation of ensuring any data we read does not get rolled back
    // relies on the fact that we read from a single WT snapshot, since we
    // choose the timestamp to wait on in the first command of the
    // transaction.)
    let assertSameMembers = (members) => {
        assert.sameMembers(members, sessionColl.find().toArray());
    };

    assertSameMembers([{_id: 0}]);
    assert.commandWorked(testColl.insert({_id: 1}));
    assertSameMembers([{_id: 0}]);
    assert.commandWorked(testColl.insert({_id: 2}, {writeConcern: {w: "majority"}}));
    assertSameMembers([{_id: 0}]);

    assert.commandWorked(session.commitTransaction_forTesting());

    assertSameMembers([{_id: 0}, {_id: 1}, {_id: 2}]);
    session.endSession();
}());
