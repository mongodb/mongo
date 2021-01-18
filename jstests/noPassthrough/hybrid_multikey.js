/**
 * Tests that hybrid index builds result in a consistent state and correct multikey behavior across
 * various index types.
 */
load("jstests/noPassthrough/libs/index_build.js");
load("jstests/libs/analyze_plan.js");  // For getWinningPlan to analyze explain() output.

(function() {
"use strict";

const conn = MongoRunner.runMongod();
const dbName = 'test';
const collName = 'foo';
const testDB = conn.getDB(dbName);
const testColl = testDB[collName];

const runTest = (config) => {
    // Populate the collection.
    const nDocs = 10;
    testColl.drop();
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(testColl.insert({x: i}));
    }

    jsTestLog("Testing: " + tojson(config));

    // Prevent the index build from completing.
    IndexBuildTest.pauseIndexBuilds(conn);

    // Start the index build and wait for it to start.
    const indexName = "test_index";
    let indexOptions = config.indexOptions || {};
    indexOptions.name = indexName;
    const awaitIndex = IndexBuildTest.startIndexBuild(
        conn, testColl.getFullName(), config.indexSpec, indexOptions);
    IndexBuildTest.waitForIndexBuildToStart(testDB, collName, indexName);

    // Perform writes while the index build is in progress.
    assert.commandWorked(testColl.insert({x: nDocs + 1}));
    assert.commandWorked(testColl.update({x: 0}, {x: -1}));
    assert.commandWorked(testColl.remove({x: -1}));

    let extraDocs = config.extraDocs || [];
    extraDocs.forEach((doc) => {
        assert.commandWorked(testColl.insert(doc));
    });

    // Allow the index build to complete.
    IndexBuildTest.resumeIndexBuilds(conn);
    awaitIndex();

    // Ensure the index is usable and has the expected multikey state.
    let explain = testColl.find({x: 1}).hint(indexName).explain();
    let plan = getWinningPlan(explain.queryPlanner);
    assert.eq("FETCH", plan.stage, explain);
    assert.eq("IXSCAN", plan.inputStage.stage, explain);
    assert.eq(
        config.expectMultikey,
        plan.inputStage.isMultiKey,
        `Index multikey state "${plan.inputStage.isMultiKey}" was not "${config.expectMultikey}"`);

    const validateRes = assert.commandWorked(testColl.validate());
    assert(validateRes.valid, validateRes);
};

// Hashed indexes should never be multikey.
runTest({
    indexSpec: {x: 'hashed'},
    expectMultikey: false,
});

// Wildcard indexes are not multikey unless they have multikey documents.
runTest({
    indexSpec: {'$**': 1},
    expectMultikey: false,
});
runTest({
    indexSpec: {'$**': 1},
    expectMultikey: true,
    extraDocs: [{x: [1, 2, 3]}],
});

// '2dsphere' indexes are not multikey unless they have multikey documents.
runTest({
    indexSpec: {x: 1, b: '2dsphere'},
    expectMultikey: false,
});

// '2dsphere' indexes are automatically sparse. If we insert a document where 'x' is multikey, even
// though 'b' is omitted, the index is still considered multikey. See SERVER-39705.
runTest({
    indexSpec: {x: 1, b: '2dsphere'},
    extraDocs: [
        {x: [1, 2]},
    ],
    expectMultikey: true,
});

// Test that a partial index is not multikey when a multikey document is not indexed.
runTest({
    indexSpec: {x: 1},
    indexOptions: {partialFilterExpression: {a: {$gt: 0}}},
    extraDocs: [
        {x: [0, 1, 2], a: 0},
    ],
    expectMultikey: false,
});

// Test that a partial index is multikey when a multikey document is indexed.
runTest({
    indexSpec: {x: 1},
    indexOptions: {partialFilterExpression: {a: {$gt: 0}}},
    extraDocs: [
        {x: [0, 1, 2], a: 1},
    ],
    expectMultikey: true,
});

// Text indexes are not multikey unless documents make them multikey.
runTest({
    indexSpec: {x: 'text'},
    extraDocs: [
        {x: 'hello'},
    ],
    expectMultikey: false,
});

// Text indexes can be multikey if a field has multiple words.
runTest({
    indexSpec: {x: 'text'},
    extraDocs: [
        {x: 'hello world'},
    ],
    expectMultikey: true,
});

MongoRunner.stopMongod(conn);
})();
