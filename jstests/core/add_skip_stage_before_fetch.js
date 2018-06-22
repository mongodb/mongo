// SERVER-13946: When possible, place skip stages before fetch stages to avoid unnecessarily
// fetching documents that will be immediately discarded.

// The skip operation in a sharded query always occurs in the mongoS, so this test doesn't make
// sense on a sharded collection.
// @tags: [assumes_unsharded_collection]

(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    const coll = db.add_skip_stage_before_fetch;

    coll.drop();
    const testIndex = {a: 1, b: 1, c: 1};
    assert.commandWorked(coll.createIndex(testIndex));

    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 10000; i++) {
        bulk.insert({
            a: i % 2,
            b: i % 4,
            c: Math.floor(Math.random() * 1000),
            d: Math.floor(Math.random() * 1000)
        });
    }
    assert.writeOK(bulk.execute());

    // The {a: 0, b: 2} query will match exactly one quarter of the documents in the collection:
    // 2500 in total. In the test queries below, we skip the first 2400, returning exactly 100
    // documents.

    // This find can be computed using the index, so we should only need to fetch the 100 documents
    // that get returned to the client after skipping the first 2400.
    let explainResult =
        coll.find({a: 0, b: 2}).hint(testIndex).skip(2400).explain("executionStats");
    assert.gte(explainResult.executionStats.totalKeysExamined, 2500);
    assert.eq(explainResult.executionStats.totalDocsExamined, 100);

    // This sort can also be computed using the index.
    explainResult =
        coll.find({a: 0, b: 2}).hint(testIndex).sort({c: 1}).skip(2400).explain("executionStats");
    assert.gte(explainResult.executionStats.totalKeysExamined, 2500);
    assert.eq(explainResult.executionStats.totalDocsExamined, 100);

    // This query is covered by the index, so there should be no fetch at all.
    explainResult = coll.find({a: 0, b: 2}, {_id: 0, a: 1})
                        .hint(testIndex)
                        .sort({c: 1})
                        .skip(2400)
                        .explain("executionStats");
    assert.gte(explainResult.executionStats.totalKeysExamined, 2500);
    assert.eq(explainResult.executionStats.totalDocsExamined, 0);
    assert(isIndexOnly(explainResult.queryPlanner.winningPlan));

    // This sort requires a field that is not in the index, so we should be fetching all 2500
    // documents that match the find predicate.
    explainResult =
        coll.find({a: 0, b: 2}).hint(testIndex).sort({d: 1}).skip(2400).explain("executionStats");
    assert.gte(explainResult.executionStats.totalKeysExamined, 2500);
    assert.eq(explainResult.executionStats.totalDocsExamined, 2500);
})();
