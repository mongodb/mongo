/*
 * This test makes sure that regex control characters in the namespace of changestream targets don't
 * affect what documents appear in a changestream, in response to SERVER-41164.
 */
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");
load("jstests/libs/collection_drop_recreate.js");
function test_no_leak(dbNameUnrelated, collNameUnrelated, dbNameProblematic, collNameProblematic) {
    const dbUnrelated = db.getSiblingDB(dbNameUnrelated);
    const cstUnrelated = new ChangeStreamTest(dbUnrelated);
    assertDropAndRecreateCollection(dbUnrelated, collNameUnrelated);

    const watchUnrelated = cstUnrelated.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: collNameUnrelated});

    const dbProblematic = db.getSiblingDB(dbNameProblematic);
    const cstProblematic = new ChangeStreamTest(dbProblematic);
    assertDropAndRecreateCollection(dbProblematic, collNameProblematic);

    const watchProblematic = cstProblematic.startWatchingChanges(
        {pipeline: [{$changeStream: {}}], collection: collNameProblematic});

    assert.commandWorked(dbUnrelated.getCollection(collNameUnrelated).insert({_id: 2}));
    let expected = {
        documentKey: {_id: 2},
        fullDocument: {_id: 2},
        ns: {db: dbNameUnrelated, coll: collNameUnrelated},
        operationType: "insert",
    };
    // Make sure that only the database which was inserted into reflects a change on its
    // changestream.
    cstUnrelated.assertNextChangesEqual({cursor: watchUnrelated, expectedChanges: [expected]});
    // The other DB shouldn't have any changes.
    cstProblematic.assertNoChange(watchProblematic);

    assert.commandWorked(dbProblematic.getCollection(collNameProblematic).insert({_id: 3}));
    expected = {
        documentKey: {_id: 3},
        fullDocument: {_id: 3},
        ns: {db: dbNameProblematic, coll: collNameProblematic},
        operationType: "insert",
    };
    cstProblematic.assertNextChangesEqual({cursor: watchProblematic, expectedChanges: [expected]});
    cstUnrelated.assertNoChange(watchUnrelated);

    cstUnrelated.cleanUp();
    cstProblematic.cleanUp();
}
if (!_isWindows()) {
    test_no_leak("has_no_pipe", "coll", "has_a_|pipe", "coll");
    test_no_leak("starssss", "coll", "stars*", "coll");
}
test_no_leak("has_[two]_brackets", "coll", "has_t_brackets", "coll");
test_no_leak("test", "dotted.collection", "testadotted", "collection");
test_no_leak("carat", "coll", "hasa^carat", "coll");
test_no_leak("db1", "coll", "db1", "col*");
}());
