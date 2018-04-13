/**
 * Tests that change streams can be opened on a namespace before the collection or database has been
 * created, and will not implicitly create either.
 */

(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");  // For 'ChangeStreamTest'.

    // Ensure that the test DB does not exist.
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    let dbList = assert.commandWorked(
        db.adminCommand({listDatabases: 1, nameOnly: true, filter: {name: testDB.getName()}}));
    assert.docEq(dbList.databases, []);

    const collName = "test";

    // Start a new $changeStream on the non-existent db.
    const cst = new ChangeStreamTest(testDB);
    const changeStreamCursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: collName});

    // Confirm that a $changeStream cursor has been opened on the namespace.
    assert.gt(changeStreamCursor.id, 0);

    // Confirm that the database has not been implicitly created.
    dbList = assert.commandWorked(
        db.adminCommand({listDatabases: 1, nameOnly: true, filter: {name: testDB.getName()}}));
    assert.docEq(dbList.databases, []);

    // Confirm that a non-$changeStream aggregation on the non-existent database returns an empty
    // cursor.
    const nonCsCmdRes = assert.commandWorked(
        testDB.runCommand({aggregate: collName, pipeline: [{$match: {}}], cursor: {}}));
    assert.docEq(nonCsCmdRes.cursor.firstBatch, []);
    assert.eq(nonCsCmdRes.cursor.id, 0);

    // Now perform some writes into the collection...
    assert.commandWorked(testDB[collName].insert({_id: 1}));
    assert.commandWorked(testDB[collName].insert({_id: 2}));
    assert.commandWorked(testDB[collName].update({_id: 1}, {$set: {updated: true}}));
    assert.commandWorked(testDB[collName].remove({_id: 2}));

    // ... confirm that the database has been created...
    dbList = assert.commandWorked(
        db.adminCommand({listDatabases: 1, nameOnly: true, filter: {name: testDB.getName()}}));
    assert.docEq(dbList.databases, [{name: testDB.getName()}]);

    // ... and verify that the changes are observed by the stream.
    const expectedChanges = [
        {
          documentKey: {_id: 1},
          fullDocument: {_id: 1},
          ns: {db: testDB.getName(), coll: collName},
          operationType: "insert"
        },
        {
          documentKey: {_id: 2},
          fullDocument: {_id: 2},
          ns: {db: testDB.getName(), coll: collName},
          operationType: "insert"
        },
        {
          documentKey: {_id: 1},
          ns: {db: testDB.getName(), coll: collName},
          updateDescription: {removedFields: [], updatedFields: {updated: true}},
          operationType: "update"
        },
        {
          documentKey: {_id: 2},
          ns: {db: testDB.getName(), coll: collName},
          operationType: "delete"
        },
    ];

    cst.assertNextChangesEqual({cursor: changeStreamCursor, expectedChanges: expectedChanges});
    cst.cleanUp();
})();