// @tags: [requires_getmore, assumes_balancer_off]

// Test that even when the execution of a query fails, explain reports query
// planner information.

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

var t = db.explain_execution_error;
t.drop();

var result;

/**
 * Asserts that explain reports an error in its execution stats section.
 */
function assertExecError(explain) {
    // Gather the exec stats from all shards.
    let allExecStats = [];
    let topLevelExecStats = explain.executionStats;
    if (topLevelExecStats.executionStages.stage == "SINGLE_SHARD" ||
        topLevelExecStats.executionStages.stage == "SHARD_MERGE_SORT") {
        allExecStats = topLevelExecStats.executionStages.shards;
    } else {
        allExecStats.push(topLevelExecStats);
    }

    // In a sharded environment, we only know that at least one of the shards will fail, we can't
    // expect all of them to fail, since there may be different amounts of data on each shard.
    let haveSeenExecutionFailure = false;
    for (let execStats of allExecStats) {
        if (!execStats.executionSuccess) {
            haveSeenExecutionFailure = true;
            assert("errorMessage" in execStats,
                   `Expected "errorMessage" to be present in ${tojson(execStats)}`);
            assert("errorCode" in execStats,
                   `Expected "errorCode" to be present in ${tojson(execStats)}`);
        }
    }
    assert(haveSeenExecutionFailure,
           `Expected at least one shard to have failed: ${tojson(explain)}`);
}

/**
 * Asserts that explain reports success in its execution stats section.
 */
function assertExecSuccess(explain) {
    let errorObjs = [];

    let execStats = explain.executionStats;
    if (execStats.executionStages.stage == "SINGLE_SHARD" ||
        execStats.executionStages.stage == "SHARD_MERGE_SORT") {
        errorObjs = execStats.executionStages.shards;
    } else {
        errorObjs.push(execStats);
    }

    for (let errorObj of errorObjs) {
        assert.eq(true, errorObj.executionSuccess);
        assert(!("errorMessage" in errorObj),
               `Expected "errorMessage" not to be present in ${tojson(errorObj)}`);
        assert(!("errorCode" in errorObj),
               `Expected "errorCode" not to be present in ${tojson(errorObj)}`);
    }
}

// Make a string that exceeds 1 MB.
var bigStr = "x";
while (bigStr.length < (1024 * 1024)) {
    bigStr += bigStr;
}

// Make a collection that is about 40 MB * number of shards.
const numShards = FixtureHelpers.numberOfShardsForCollection(t);
for (var i = 0; i < 40 * numShards; i++) {
    assert.writeOK(t.insert({a: bigStr, b: 1, c: i}));
}

// A query which sorts the whole collection by "b" should throw an error due to hitting the
// memory limit for sort.
assert.throws(function() {
    t.find({a: {$exists: true}}).sort({b: 1}).itcount();
});

// Explain of this query should succeed at query planner verbosity.
result = db.runCommand({
    explain: {find: t.getName(), filter: {a: {$exists: true}}, sort: {b: 1}},
    verbosity: "queryPlanner"
});
assert.commandWorked(result);
assert("queryPlanner" in result);

// Explaining the same query at execution stats verbosity should succeed, but indicate that the
// underlying operation failed.
result = db.runCommand({
    explain: {find: t.getName(), filter: {a: {$exists: true}}, sort: {b: 1}},
    verbosity: "executionStats"
});
assert.commandWorked(result);
assert("queryPlanner" in result);
assert("executionStats" in result);
assertExecError(result);

// The underlying operation should also report a failure at allPlansExecution verbosity.
result = db.runCommand({
    explain: {find: t.getName(), filter: {a: {$exists: true}}, sort: {b: 1}},
    verbosity: "allPlansExecution"
});
assert.commandWorked(result);
assert("queryPlanner" in result);
assert("executionStats" in result);
assert("allPlansExecution" in result.executionStats);
assertExecError(result);

// Now we introduce two indices. One provides the requested sort order, and
// the other does not.
t.ensureIndex({b: 1});
t.ensureIndex({c: 1});

// The query should no longer fail with a memory limit error because the planner can obtain
// the sort by scanning an index.
assert.eq(40, t.find({c: {$lt: 40}}).sort({b: 1}).itcount());

// The explain should succeed at all verbosity levels because the query itself succeeds.
// First test "queryPlanner" verbosity.
result = db.runCommand({
    explain: {find: t.getName(), filter: {c: {$lt: 40}}, sort: {b: 1}},
    verbosity: "queryPlanner"
});
assert.commandWorked(result);
assert("queryPlanner" in result);

result = db.runCommand({
    explain: {find: t.getName(), filter: {c: {$lt: 40}}, sort: {b: 1}},
    verbosity: "executionStats"
});
assert.commandWorked(result);
assert("queryPlanner" in result);
assert("executionStats" in result);
assertExecSuccess(result);

// We expect allPlansExecution verbosity to show execution stats for both candidate plans.
result = db.runCommand({
    explain: {find: t.getName(), filter: {c: {$lt: 40}}, sort: {b: 1}},
    verbosity: "allPlansExecution"
});
assert.commandWorked(result);
assert("queryPlanner" in result);
assert("executionStats" in result);
assert("allPlansExecution" in result.executionStats);
assertExecSuccess(result);
