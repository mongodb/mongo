// Test that queryExecStats within a $collStats stage returns the correct execution stats.
// @tags: [assumes_no_implicit_collection_creation_after_drop]
(function() {
    "use strict";

    if (jsTest.options().storageEngine === "mobile") {
        print("Skipping test because storage engine isn't mobile");
        return;
    }

    load("jstests/aggregation/extras/utils.js");  // For "assertErrorCode".
    load("jstests/libs/fixture_helpers.js");      // For "FixtureHelpers".

    const nDocs = 32;

    const testDB = db.getSiblingDB("aggregation_query_exec_stats");
    const coll = testDB.aggregation_query_exec_stats;
    coll.drop();
    assert.commandWorked(
        testDB.createCollection("aggregation_query_exec_stats", {capped: true, size: nDocs * 100}));

    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.insert({a: i}));
    }

    // Run a bunch of collection scans on the server.
    for (let i = 0; i < nDocs; i++) {
        assert.eq(coll.find({a: i}).itcount(), 1);
    }

    // Test that an error is returned if queryExecStats is not an object.
    let pipeline = [{$collStats: {queryExecStats: 1}}];
    assertErrorCode(coll, pipeline, 31141, "queryExecStats spec must be an object");
    pipeline = [{$collStats: {queryExecStats: "1"}}];
    assertErrorCode(coll, pipeline, 31141, "queryExecStats spec must be an object");

    // Test the accuracy of the result of queryExecStats as a standalone option.
    pipeline = [{$collStats: {queryExecStats: {}}}];
    let result = coll.aggregate(pipeline).next();
    assert.eq(nDocs, result.queryExecStats.collectionScans.total);
    assert.eq(nDocs, result.queryExecStats.collectionScans.nonTailable);

    // Test tailable collection scans update collectionScans counters appropriately.
    for (let i = 0; i < nDocs; i++) {
        assert.eq(coll.find({a: i}).tailable().itcount(), 1);
    }
    result = coll.aggregate(pipeline).next();
    assert.eq(nDocs * 2, result.queryExecStats.collectionScans.total);
    assert.eq(nDocs, result.queryExecStats.collectionScans.nonTailable);

    // Run a query which will require the client to fetch multiple batches from the server. Ensure
    // that the getMore commands don't increment the counter of collection scans.
    assert.eq(coll.find({}).batchSize(2).itcount(), nDocs);
    result = coll.aggregate(pipeline).next();
    assert.eq((nDocs * 2) + 1, result.queryExecStats.collectionScans.total);
    assert.eq(nDocs + 1, result.queryExecStats.collectionScans.nonTailable);

    // Create index to test that index scans don't up the collection scan counter.
    assert.commandWorked(coll.createIndex({a: 1}));
    // Run a bunch of index scans.
    for (let i = 0; i < nDocs; i++) {
        assert.eq(coll.find({a: i}).itcount(), 1);
    }
    result = coll.aggregate(pipeline).next();
    // Assert that the number of collection scans hasn't increased.
    assert.eq((nDocs * 2) + 1, result.queryExecStats.collectionScans.total);
    assert.eq(nDocs + 1, result.queryExecStats.collectionScans.nonTailable);

    // Test that we error when the collection does not exist.
    coll.drop();
    pipeline = [{$collStats: {queryExecStats: {}}}];
    assertErrorCode(coll, pipeline, 31142);

    // Test that we error when the database does not exist.
    // TODO SERVER-33039 When running against a mongos, a non-existent database will cause all
    // aggregations to return an empty result set.
    assert.commandWorked(testDB.dropDatabase());
    if (FixtureHelpers.isMongos(testDB)) {
        assert.eq([], coll.aggregate(pipeline).toArray());
    } else {
        assertErrorCode(coll, pipeline, 31142);
    }
}());
