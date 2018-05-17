/**
 * Make sure the 'exhaust' query option is not able to be used in a session.
 */
(function() {
    "use strict";

    let conn = MongoRunner.runMongod();

    const dbName = 'test';
    const collName = 'coll';

    const session = conn.startSession();
    const sessionColl = session.getDatabase(dbName).getCollection(collName);
    const testColl = conn.getDB(dbName).getCollection(collName);

    testColl.drop();

    // Create a collection to query.
    assert.commandWorked(testColl.insert({_id: 1}));

    // Exhaust outside of session should work.
    let docs = testColl.find().addOption(DBQuery.Option.exhaust).toArray();
    assert.docEq([{_id: 1}], docs);

    // Exhaust in session should fail.
    assert.throws(() => {
        sessionColl.find().addOption(DBQuery.Option.exhaust).toArray();
    });

    session.endSession();
    MongoRunner.stopMongod(conn);
}());
