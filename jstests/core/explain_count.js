// Test running explains on count commands.

load("jstests/libs/analyze_plan.js");

var collName = "jstests_explain_count";
var t = db[collName];
t.drop();

/**
 * Given explain output 'explain' at executionStats level verbosity,
 * confirms that the root stage is COUNT and that the result of the
 * count is equal to 'nCounted'.
 */
function checkCountExplain(explain, nCounted) {
    printjson(explain);
    var execStages = explain.executionStats.executionStages;

    // If passed through mongos, then the root stage should be the mongos SINGLE_SHARD stage,
    // with COUNT as its child. If explaining directly on the shard, then COUNT is the root
    // stage.
    if ("SINGLE_SHARD" == execStages.stage) {
        var countStage = execStages.shards[0].executionStages;
        assert.eq(countStage.stage, "COUNT", "root stage on shard is not COUNT");
        assert.eq(countStage.nCounted, nCounted, "wrong count result");
    } else {
        assert.eq(execStages.stage, "COUNT", "root stage is not COUNT");
        assert.eq(execStages.nCounted, nCounted, "wrong count result");
    }
}

/**
 * Given an explain output from a COUNT_SCAN stage, check that a indexBounds field is present.
 */
function checkCountScanIndexExplain(explain, startKey, endKey, startInclusive, endInclusive) {
    var countStage = getPlanStage(explain.executionStats.executionStages, "COUNT_SCAN");

    assert.eq(countStage.stage, "COUNT_SCAN");
    assert("indexBounds" in countStage);
    assert.eq(bsonWoCompare(countStage.indexBounds.startKey, startKey), 0);
    assert.eq(bsonWoCompare(countStage.indexBounds.endKey, endKey), 0);
    assert.eq(countStage.indexBounds.startKeyInclusive, startInclusive);
    assert.eq(countStage.indexBounds.endKeyInclusive, endInclusive);
}

// Collection does not exist.
assert.eq(0, t.count());
var explain = db.runCommand({explain: {count: collName}, verbosity: "executionStats"});
checkCountExplain(explain, 0);

// Collection does not exist with skip, limit, and/or query.
assert.eq(0, db.runCommand({count: collName, skip: 3}).n);
explain = db.runCommand({explain: {count: collName, skip: 3}, verbosity: "executionStats"});
checkCountExplain(explain, 0);

assert.eq(0, db.runCommand({count: collName, limit: 3}).n);
explain = db.runCommand({explain: {count: collName, limit: 3}, verbosity: "executionStats"});
checkCountExplain(explain, 0);

assert.eq(0, db.runCommand({count: collName, limit: -3}).n);
explain = db.runCommand({explain: {count: collName, limit: -3}, verbosity: "executionStats"});
checkCountExplain(explain, 0);

assert.eq(0, db.runCommand({count: collName, limit: -3, skip: 4}).n);
explain =
    db.runCommand({explain: {count: collName, limit: -3, skip: 4}, verbosity: "executionStats"});
checkCountExplain(explain, 0);

assert.eq(0, db.runCommand({count: collName, query: {a: 1}, limit: -3, skip: 4}).n);
explain = db.runCommand(
    {explain: {count: collName, query: {a: 1}, limit: -3, skip: 4}, verbosity: "executionStats"});
checkCountExplain(explain, 0);

// Now add a bit of data to the collection.
t.ensureIndex({a: 1});
for (var i = 0; i < 10; i++) {
    t.insert({_id: i, a: 1});
}

// Trivial count with no skip, limit, or query.
assert.eq(10, t.count());
explain = db.runCommand({explain: {count: collName}, verbosity: "executionStats"});
checkCountExplain(explain, 10);

// Trivial count with skip.
assert.eq(7, db.runCommand({count: collName, skip: 3}).n);
explain = db.runCommand({explain: {count: collName, skip: 3}, verbosity: "executionStats"});
checkCountExplain(explain, 7);

// Trivial count with limit.
assert.eq(3, db.runCommand({count: collName, limit: 3}).n);
explain = db.runCommand({explain: {count: collName, limit: 3}, verbosity: "executionStats"});
checkCountExplain(explain, 3);

// Trivial count with negative limit.
assert.eq(3, db.runCommand({count: collName, limit: -3}).n);
explain = db.runCommand({explain: {count: collName, limit: -3}, verbosity: "executionStats"});
checkCountExplain(explain, 3);

// Trivial count with both limit and skip.
assert.eq(3, db.runCommand({count: collName, limit: -3, skip: 4}).n);
explain =
    db.runCommand({explain: {count: collName, limit: -3, skip: 4}, verbosity: "executionStats"});
checkCountExplain(explain, 3);

// With a query.
assert.eq(10, db.runCommand({count: collName, query: {a: 1}}).n);
explain = db.runCommand({explain: {count: collName, query: {a: 1}}, verbosity: "executionStats"});
checkCountExplain(explain, 10);
checkCountScanIndexExplain(explain, {a: 1}, {a: 1}, true, true);

// With a query and skip.
assert.eq(7, db.runCommand({count: collName, query: {a: 1}, skip: 3}).n);
explain = db.runCommand(
    {explain: {count: collName, query: {a: 1}, skip: 3}, verbosity: "executionStats"});
checkCountExplain(explain, 7);
checkCountScanIndexExplain(explain, {a: 1}, {a: 1}, true, true);

// With a query and limit.
assert.eq(3, db.runCommand({count: collName, query: {a: 1}, limit: 3}).n);
explain = db.runCommand(
    {explain: {count: collName, query: {a: 1}, limit: 3}, verbosity: "executionStats"});
checkCountExplain(explain, 3);
checkCountScanIndexExplain(explain, {a: 1}, {a: 1}, true, true);

// Insert one more doc for the last few tests.
t.insert({a: 2});

// Case where all results are skipped.
assert.eq(0, db.runCommand({count: collName, query: {a: 2}, skip: 2}).n);
explain = db.runCommand(
    {explain: {count: collName, query: {a: 2}, skip: 2}, verbosity: "executionStats"});
checkCountExplain(explain, 0);
checkCountScanIndexExplain(explain, {a: 2}, {a: 2}, true, true);

// Case where we have a limit, but we don't hit it.
assert.eq(1, db.runCommand({count: collName, query: {a: 2}, limit: 2}).n);
explain = db.runCommand(
    {explain: {count: collName, query: {a: 2}, limit: 2}, verbosity: "executionStats"});
checkCountExplain(explain, 1);
checkCountScanIndexExplain(explain, {a: 2}, {a: 2}, true, true);
