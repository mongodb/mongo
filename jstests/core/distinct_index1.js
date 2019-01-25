/**
 * Analyzes execution stats for indexed distinct.
 * @tags: [assumes_balancer_off]
 */
(function() {
    load("jstests/libs/analyze_plan.js");  // For getPlanStage.

    const coll = db.distinct_index1;
    coll.drop();

    function getHash(num) {
        return Math.floor(Math.sqrt(num * 123123)) % 10;
    }

    function getDistinctExplainWithExecutionStats(field, query) {
        const explain = coll.explain("executionStats").distinct(field, query || {});
        assert(explain.hasOwnProperty("executionStats"), explain);
        return explain;
    }

    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 1000; i++) {
        bulk.insert({a: getHash(i * 5), b: getHash(i)});
    }
    assert.commandWorked(bulk.execute());

    let explain = getDistinctExplainWithExecutionStats("a");
    // Collection scan looks at all 1000 documents and gets 1000 distinct values. Looks at 0 index
    // keys.
    assert.eq(1000, explain.executionStats.nReturned);
    assert.eq(0, explain.executionStats.totalKeysExamined);
    assert.eq(1000, explain.executionStats.totalDocsExamined);

    explain = getDistinctExplainWithExecutionStats("a", {a: {$gt: 5}});
    // Collection scan looks at all 1000 documents and gets 398 distinct values which match the
    // query. Looks at 0 index keys.
    assert.eq(398, explain.executionStats.nReturned);
    assert.eq(0, explain.executionStats.totalKeysExamined);
    assert.eq(1000, explain.executionStats.totalDocsExamined);

    explain = getDistinctExplainWithExecutionStats("b", {a: {$gt: 5}});
    // Collection scan looks at all 1000 documents and gets 398 distinct values which match the
    // query. Looks at 0 index keys.
    assert.eq(398, explain.executionStats.nReturned);
    assert.eq(0, explain.executionStats.totalKeysExamined);
    assert.eq(1000, explain.executionStats.totalDocsExamined);

    assert.commandWorked(coll.createIndex({a: 1}));

    explain = getDistinctExplainWithExecutionStats("a");
    // There are only 10 values.  We use the fast distinct hack and only examine each value once.
    assert.eq(10, explain.executionStats.nReturned);
    assert.lte(10, explain.executionStats.totalKeysExamined);

    explain = getDistinctExplainWithExecutionStats("a", {a: {$gt: 5}});
    // Only 4 values of a are >= 5 and we use the fast distinct hack.
    assert.eq(4, explain.executionStats.nReturned);
    assert.eq(4, explain.executionStats.totalKeysExamined);
    assert.eq(0, explain.executionStats.totalDocsExamined);

    explain = getDistinctExplainWithExecutionStats("b", {a: {$gt: 5}});
    // We can't use the fast distinct hack here because we're distinct-ing over 'b'.
    assert.eq(398, explain.executionStats.nReturned);
    assert.eq(398, explain.executionStats.totalKeysExamined);
    assert.eq(398, explain.executionStats.totalDocsExamined);

    // Test that a distinct over a trailing field of the index can be covered.
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    explain = getDistinctExplainWithExecutionStats("b", {a: {$gt: 5}, b: {$gt: 5}});
    assert.lte(explain.executionStats.nReturned, 171);
    assert.eq(0, explain.executionStats.totalDocsExamined);

    // Should use an index scan over the hashed index.
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: "hashed"}));
    explain = getDistinctExplainWithExecutionStats("a", {$or: [{a: 3}, {a: 5}]});
    assert.eq(188, explain.executionStats.nReturned);
    const indexScanStage = getPlanStage(explain.executionStats.executionStages, "IXSCAN");
    assert.eq("hashed", indexScanStage.keyPattern.a);
})();
