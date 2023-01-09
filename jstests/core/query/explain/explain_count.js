// Test running explains on count commands.
//
// @tags: [requires_fastcount]

load("jstests/libs/analyze_plan.js");     // For assertExplainCount.
load("jstests/libs/fixture_helpers.js");  // For isMongos and isSharded.

var collName = "jstests_explain_count";
var t = db[collName];
t.drop();

/**
 * Given an explain output from a COUNT_SCAN stage, check that a indexBounds field is present.
 */
function checkCountScanIndexExplain(explain, startKey, endKey, startInclusive, endInclusive) {
    var countStage = getPlanStage(explain.executionStats.executionStages, "COUNT_SCAN");

    assert.eq(countStage.stage, "COUNT_SCAN");
    assert("indexBounds" in countStage);
    assert.eq(countStage.indexBounds.startKey, startKey);
    assert.eq(countStage.indexBounds.endKey, endKey);
    assert.eq(countStage.indexBounds.startKeyInclusive, startInclusive);
    assert.eq(countStage.indexBounds.endKeyInclusive, endInclusive);
}

/**
 * Ensure that the SHARDING_FILTER's child stage is an IXSCAN (and not a fetch). This is to ensure
 * sharded clusters can still run the count command with a predicate on indexed fields reasonably
 * fast. Assumes that the shard key is part of the index.
 */
function checkShardingFilterIndexScanExplain(explain, keyName, bounds) {
    var filterStage = getPlanStage(explain.executionStats.executionStages, "SHARDING_FILTER");

    assert.eq(filterStage.stage, "SHARDING_FILTER");
    const ixScanStage = filterStage.inputStage;
    assert.eq(ixScanStage.stage, "IXSCAN");
    assert("indexBounds" in ixScanStage);

    assert.eq(ixScanStage.indexBounds[keyName].length, 1);
    const expectedBoundsArr = JSON.parse(ixScanStage.indexBounds[keyName][0]);
    assert.eq(expectedBoundsArr, bounds);
}

/**
 * Check that the explain from a count command run on a collection with a usable index for the
 * predicate produces a reasonable plan. On sharded collections, we expect to have an IXSCAN
 * followed by a SHARDING_FILTER. Otherwise, the COUNT_SCAN stage should be used.
 */
function checkIndexedCountWithPred(db, explain, keyName, bounds) {
    assert.eq(bounds.length, 2);
    if (isMongos(db) && FixtureHelpers.isSharded(db[collName])) {
        // On sharded collections we have a SHARDING_FILTER with a child that's an IXSCAN.
        checkShardingFilterIndexScanExplain(explain, keyName, bounds);
    } else {
        // On a standalone we just do a COUNT_SCAN over the {a: 1, _id: 1} index.
        const min = {a: bounds[0], _id: {"$minKey": 1}};
        const max = {a: bounds[1], _id: {"$maxKey": 1}};
        checkCountScanIndexExplain(explain, min, max, true, true);
    }
}

// Collection does not exist.
assert.eq(0, t.count());
var explain =
    assert.commandWorked(db.runCommand({explain: {count: collName}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 0});

// Collection does not exist with skip, limit, and/or query.
var result = assert.commandWorked(db.runCommand({count: collName, skip: 3}));
assert.eq(0, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, skip: 3}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 0});

result = assert.commandWorked(db.runCommand({count: collName, limit: 3}));
assert.eq(0, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, limit: 3}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 0});

result = assert.commandWorked(db.runCommand({count: collName, limit: -3}));
assert.eq(0, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, limit: -3}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 0});

result = assert.commandWorked(db.runCommand({count: collName, limit: -3, skip: 4}));
assert.eq(0, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, limit: -3, skip: 4}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 0});

result = assert.commandWorked(db.runCommand({count: collName, query: {a: 1}, limit: -3, skip: 4}));
assert.eq(0, result.n);
explain = assert.commandWorked(db.runCommand(
    {explain: {count: collName, query: {a: 1}, limit: -3, skip: 4}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 0});

// Now add a bit of data to the collection.
// On sharded clusters, we'll want the shard key to be indexed, so we make _id part of the index.
// This means counts will not have to fetch from the document in order to get the shard key.
t.createIndex({a: 1, _id: 1});
for (var i = 0; i < 10; i++) {
    t.insert({_id: i, a: 1});
}

// Trivial count with no skip, limit, or query.
assert.eq(10, t.count());
explain =
    assert.commandWorked(db.runCommand({explain: {count: collName}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 10});

// Trivial count with skip.
result = assert.commandWorked(db.runCommand({count: collName, skip: 3}));
assert.eq(7, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, skip: 3}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 7});

// Trivial count with limit.
result = assert.commandWorked(db.runCommand({count: collName, limit: 3}));
assert.eq(3, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, limit: 3}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 3});

// Trivial count with negative limit.
result = assert.commandWorked(db.runCommand({count: collName, limit: -3}));
assert.eq(3, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, limit: -3}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 3});

// Trivial count with both limit and skip.
result = assert.commandWorked(db.runCommand({count: collName, limit: -3, skip: 4}));
assert.eq(3, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, limit: -3, skip: 4}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 3});

// With a query.
result = assert.commandWorked(db.runCommand({count: collName, query: {a: 1}}));
assert.eq(10, result.n);
explain = assert.commandWorked(
    db.runCommand({explain: {count: collName, query: {a: 1}}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 10});
checkIndexedCountWithPred(db, explain, "a", [1.0, 1.0]);

// With a query and skip.
result = assert.commandWorked(db.runCommand({count: collName, query: {a: 1}, skip: 3}));
assert.eq(7, result.n);
explain = assert.commandWorked(db.runCommand(
    {explain: {count: collName, query: {a: 1}, skip: 3}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 7});
checkIndexedCountWithPred(db, explain, "a", [1.0, 1.0]);

// With a query and limit.
result = assert.commandWorked(db.runCommand({count: collName, query: {a: 1}, limit: 3}));
assert.eq(3, result.n);
explain = assert.commandWorked(db.runCommand(
    {explain: {count: collName, query: {a: 1}, limit: 3}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 3});
checkIndexedCountWithPred(db, explain, "a", [1.0, 1.0]);

// Insert one more doc for the last few tests.
t.insert({a: 2});

// Case where all results are skipped.
result = assert.commandWorked(db.runCommand({count: collName, query: {a: 2}, skip: 2}));
assert.eq(0, result.n);
explain = assert.commandWorked(db.runCommand(
    {explain: {count: collName, query: {a: 2}, skip: 2}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 0});
checkIndexedCountWithPred(db, explain, "a", [2, 2]);

// Case where we have a limit, but we don't hit it.
result = assert.commandWorked(db.runCommand({count: collName, query: {a: 2}, limit: 2}));
assert.eq(1, result.n);
explain = assert.commandWorked(db.runCommand(
    {explain: {count: collName, query: {a: 2}, limit: 2}, verbosity: "executionStats"}));
assertExplainCount({explainResults: explain, expectedCount: 1});
checkIndexedCountWithPred(db, explain, "a", [2, 2]);
