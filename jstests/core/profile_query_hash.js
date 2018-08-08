// @tags: [does_not_support_stepdowns, requires_profiling]

// Confirms that profile entries for find commands contain the appropriate query hash.

(function() {
    "use strict";

    // For getLatestProfilerEntry
    load("jstests/libs/profiler.js");

    const testDB = db.getSiblingDB("query_hash");
    assert.commandWorked(testDB.dropDatabase());

    const coll = testDB.test;

    // Utility function to list query shapes in cache. The length of the list of query shapes
    // returned is used to validate the number of query hashes accumulated.
    function getShapes(collection) {
        const res = collection.runCommand('planCacheListQueryShapes');
        return res.shapes;
    }

    assert.writeOK(coll.insert({a: 1, b: 1}));
    assert.writeOK(coll.insert({a: 1, b: 2}));
    assert.writeOK(coll.insert({a: 1, b: 2}));
    assert.writeOK(coll.insert({a: 2, b: 2}));

    // We need two indices since we do not currently create cache entries for queries with a single
    // candidate plan.
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));

    assert.commandWorked(testDB.setProfilingLevel(2));

    // Executes query0 and gets the corresponding system.profile entry.
    assert.eq(
        1,
        coll.find({a: 1, b: 1}, {a: 1}).sort({a: -1}).comment("Query0 find command").itcount(),
        'unexpected document count');
    const profileObj0 =
        getLatestProfilerEntry(testDB, {op: "query", "command.comment": "Query0 find command"});
    assert(profileObj0.hasOwnProperty("queryHash"), tojson(profileObj0));
    let shapes = getShapes(coll);
    assert.eq(1, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');

    // Executes query1 and gets the corresponding system.profile entry.
    assert.eq(
        0,
        coll.find({a: 2, b: 1}, {a: 1}).sort({a: -1}).comment("Query1 find command").itcount(),
        'unexpected document count');
    const profileObj1 =
        getLatestProfilerEntry(testDB, {op: "query", "command.comment": "Query1 find command"});
    assert(profileObj1.hasOwnProperty("queryHash"), tojson(profileObj1));

    // Since the query shapes are the same, we only expect there to be one query shape present in
    // the plan cache commands output.
    shapes = getShapes(coll);
    assert.eq(1, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');
    assert.eq(profileObj0.queryHash, profileObj1.queryHash, 'unexpected not matching query hashes');

    // Test that the queryHash is the same in explain output for query0 and query1 as it was in
    // system.profile output.
    let explain = assert.commandWorked(coll.find({a: 1, b: 1}, {a: 1})
                                           .sort({a: -1})
                                           .comment("Query0 find command")
                                           .explain("queryPlanner"));
    assert.eq(explain.queryPlanner.queryHash, profileObj0.queryHash, () => tojson(explain));
    explain = assert.commandWorked(coll.find({a: 2, b: 1}, {a: 1})
                                       .sort({a: -1})
                                       .comment("Query1 find command")
                                       .explain("queryPlanner"));
    assert.eq(explain.queryPlanner.queryHash, profileObj0.queryHash, () => tojson(explain));

    // Executes query2 and gets the corresponding system.profile entry.
    assert.eq(0,
              coll.find({a: 12000, b: 1}).comment("Query2 find command").itcount(),
              'unexpected document count');
    const profileObj2 =
        getLatestProfilerEntry(testDB, {op: "query", "command.comment": "Query2 find command"});
    assert(profileObj2.hasOwnProperty("queryHash"), tojson(profileObj2));

    // Query0 and query1 should both have the same query hash for the given indexes. Whereas, query2
    // should have a unique hash. Asserts that a total of two distinct hashes results in two query
    // shapes.
    shapes = getShapes(coll);
    assert.eq(2, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');
    assert.neq(profileObj0.queryHash, profileObj2.queryHash, 'unexpected matching query hashes');

    // The queryHash in explain should be different for query2 than the hash from query0 and query1.
    explain = assert.commandWorked(
        coll.find({a: 12000, b: 1}).comment("Query2 find command").explain("queryPlanner"));
    assert(explain.queryPlanner.hasOwnProperty("queryHash"));
    assert.neq(explain.queryPlanner.queryHash, profileObj0.queryHash, () => tojson(explain));
    assert.eq(explain.queryPlanner.queryHash, profileObj2.queryHash, () => tojson(explain));
})();
