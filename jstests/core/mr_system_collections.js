/**
 * Tests that MapReduce fails when initiated on system collections.
 */
(function() {
    "use strict";

    const mrDB = db.getSiblingDB("mr_system_collections");
    const map = printjson;
    const reduce = printjson;

    // Drops the collections if they exist so that we can create them anew. The view is dropped with
    // runCommand rather than the shell helper to avoid implicitly re-sharding it during the
    // sharded_collections_jscore_passthrough test suite.
    mrDB.foo.drop();
    mrDB.runCommand({drop: "view"});

    assert.writeOK(mrDB.foo.insert({x: 1}));
    assert.commandWorked(mrDB.foo.createIndex({x: 1}));
    assert.commandWorked(mrDB.createView("view", "foo", []));

    // Test that the mapReduce command works on an ordinary collection.
    assert.commandWorked(
        mrDB.runCommand(
            {mapReduce: "foo", out: {merge: "bar", sharded: true}, map: map, reduce: reduce}),
        "failed to run MapReduce on a regular collection");

    // Test that mapReduce fails when initiated against a system collection.
    const systemCollections =
        ["system.views", "system.indexes", "system.profile", "system.version"];
    for (let collection of systemCollections) {
        assert.commandFailedWithCode(mrDB.runCommand({
            mapReduce: collection,
            out: {merge: "foo", sharded: true},
            map: map,
            reduce: reduce
        }),
                                     ErrorCodes.InvalidNamespace,
                                     "unexpectedly initiated MapReduce on " + collection);
    }
}());
