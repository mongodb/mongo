/**
 * Tests that a batch size of zero can be used for aggregation commands, and all data can be
 * retrieved via getMores.
 */
(function() {
    "use strict";

    const mongodOptions = {};
    const conn = MongoRunner.runMongod(mongodOptions);
    assert.neq(null, conn, "mongod failed to start with options " + tojson(mongodOptions));

    const testDB = conn.getDB("test");
    const coll = testDB[jsTest.name];
    coll.drop();

    // Test that an aggregate is successful on a non-existent collection.
    assert.eq(0,
              coll.aggregate([]).toArray().length,
              "expected no results from an aggregation on an empty collection");

    // Test that an aggregate is successful on a non-existent collection with a batchSize of 0, and
    // that a getMore will succeed with an empty result set.
    let res = assert.commandWorked(
        testDB.runCommand({aggregate: coll.getName(), pipeline: [], cursor: {batchSize: 0}}));

    let cursor = new DBCommandCursor(conn, res);
    assert.eq(
        0, cursor.itcount(), "expected no results from getMore of aggregation on empty collection");

    // Test that an aggregation can return *all* matching data via getMores if the initial aggregate
    // used a batchSize of 0.
    const nDocs = 1000;
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < nDocs; i++) {
        bulk.insert({_id: i, stringField: "string"});
    }
    assert.writeOK(bulk.execute());

    res = assert.commandWorked(
        testDB.runCommand({aggregate: coll.getName(), pipeline: [], cursor: {batchSize: 0}}));
    cursor = new DBCommandCursor(conn, res);
    assert.eq(nDocs, cursor.itcount(), "expected all results to be returned via getMores");

    // Test that an error in a getMore will destroy the cursor.
    function assertNumOpenCursors(nExpectedOpen) {
        let serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
        assert.eq(nExpectedOpen,
                  serverStatus.metrics.cursor.open.total,
                  "expected to find " + nExpectedOpen + " open cursor(s): " +
                      tojson(serverStatus.metrics.cursor));
    }

    // Issue an aggregate command that will fail *at runtime*, so the error will happen in a
    // getMore.
    assertNumOpenCursors(0);
    res = assert.commandWorked(testDB.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$project: {invalidComputation: {$add: [1, "$stringField"]}}}],
        cursor: {batchSize: 0}
    }));
    cursor = new DBCommandCursor(conn, res);
    // SERVER-28309 We should only report 1 open cursor per aggregation.
    assertNumOpenCursors(2);

    assert.throws(() => cursor.itcount(), [], "expected getMore to fail");
    assertNumOpenCursors(0);

    // Test that an error in a getMore using a $out stage will destroy the cursor. This test is
    // intended to reproduce SERVER-26608.

    // Issue an aggregate command that will fail *at runtime*, so the error will happen in a
    // getMore.
    res = assert.commandWorked(testDB.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$out: "validated_collection"}],
        cursor: {batchSize: 0}
    }));
    cursor = new DBCommandCursor(conn, res);
    // SERVER-28309 We should only report 1 open cursor per aggregation.
    assertNumOpenCursors(2);

    // Add a document validation rule to the $out collection so that insertion will fail.
    assert.commandWorked(testDB.runCommand(
        {create: "validated_collection", validator: {stringField: {$type: "int"}}}));

    assert.throws(() => cursor.itcount(), [], "expected getMore to fail");
    assertNumOpenCursors(0);
}());
