/**
 * Auto complete returns quickly if listCollections is blocked by the transaction lock.
 *
 * @tags: [uses_transactions]
 */
(function() {
    'use strict';

    function testAutoComplete() {
        // This method updates a global object with an array of strings on success.
        assert.soon(() => {
            shellAutocomplete("db.");
            return true;
        }, null, 30 * 1000);
        return __autocomplete__;
    }

    // Create a collection.
    const collName = 'listcollections_autocomplete';
    assert.commandWorked(db[collName].insertOne({}, {writeConcern: {w: 'majority'}}));

    jsTestLog("Start transaction");

    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase('test');
    const sessionColl = sessionDb[collName];
    session.startTransaction_forTesting();
    assert.commandWorked(sessionColl.insertOne({}));

    jsTestLog("Start dropDatabase in parallel shell");

    // Wait for global X lock while blocked behind transaction with global IX lock.
    var awaitShell = startParallelShell(function() {
        db.getSiblingDB("test2").dropDatabase();
    });

    jsTestLog("Wait for dropDatabase to appear in currentOp");

    assert.soon(() => {
        return db.currentOp({'command.dropDatabase': 1}).inprog;
    });

    jsTestLog("Test that autocompleting collection names fails quickly");

    let db_stuff = testAutoComplete();
    assert(!db_stuff.includes(collName),
           `Completions should not include "${collName}": ${db_stuff}`);

    // Verify we have some results despite the timeout.
    assert.contains('db.adminCommand(', db_stuff);

    jsTestLog("Abort transaction autocomplete collection names");

    assert.commandWorked(session.abortTransaction_forTesting());
    awaitShell();
    db_stuff = testAutoComplete();
    assert.contains('db.adminCommand(', db_stuff);
    assert.contains(`db.${collName}`, db_stuff);
})();
