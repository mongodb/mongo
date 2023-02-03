/**
 * Testing of the query planner heuristics for determining whether a collection is eligible for
 * column scan.
 * @tags: [
 *   # column store indexes are still under a feature flag and require full sbe
 *   featureFlagColumnstoreIndexes,
 *   featureFlagSbeFull
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");      // For "planHasStage."
load("jstests/libs/columnstore_util.js");  // For "setUpServerForColumnStoreIndexTest."
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For "setParameter."

if (!setUpServerForColumnStoreIndexTest(db)) {
    return;
}

const coll = db.columnstore_planning_heuristics;
coll.drop();
assert.commandWorked(coll.createIndex({"$**": "columnstore"}));

// Reset the given params to values such that the related check is guaranteed NOT to pass in this
// test. Since the heuristics are OR-ed together, this allows us to isolate a single threshold for
// testing.
function resetParameters(params) {
    for (const paramName of params) {
        setParameter(
            db, paramName, 1024 * 1024 * 1024);  // any large number is enough for these tests
    }
}

function assertColumnScanUsed(filter, shouldUseColumnScan, thresholdName) {
    const explain = coll.find(filter, {_id: 0, a: 1}).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN") == shouldUseColumnScan,
           `Threshold met: ${thresholdName} but column scan was${
               (shouldUseColumnScan ? " not " : " ")}used: ${tojson(explain)}`);
}

// Helper that sets the parameter to the specified value and ensures that column scan is used.
function runParameterTest(paramName, paramValue, queryFilter = {}) {
    setParameter(db, paramName, paramValue);
    assertColumnScanUsed(queryFilter, true, paramName);
    resetParameters([paramName]);
}

// Start with all thresholds set to non-passing values.
resetParameters([
    "internalQueryColumnScanMinNumColumnFilters",
    "internalQueryColumnScanMinAvgDocSizeBytes",
    "internalQueryColumnScanMinCollectionSizeBytes"
]);

// Test heuristics on an empty collection.
assertColumnScanUsed({}, false, "none");  // No thresholds met.
runParameterTest("internalQueryColumnScanMinNumColumnFilters", 0);
runParameterTest("internalQueryColumnScanMinAvgDocSizeBytes", 0);
runParameterTest("internalQueryColumnScanMinCollectionSizeBytes", 0);

// Test heuristics on a non-empty collection (content doesn't matter for this test).
for (let i = 0; i < 20; ++i) {
    coll.insert([{a: i}]);
}

assertColumnScanUsed({}, false, "none");  // No thresholds met.
runParameterTest("internalQueryColumnScanMinNumColumnFilters", 1, {a: 1});
runParameterTest("internalQueryColumnScanMinAvgDocSizeBytes", 1);
runParameterTest("internalQueryColumnScanMinCollectionSizeBytes", 1);

// Special case - use available memory as the collection size threshold.
setParameter(db, "internalQueryColumnScanMinCollectionSizeBytes", -1);
assertColumnScanUsed({}, false, "none");

// Test that a hint will still allow us to use the index.
const explain = coll.find({}, {_id: 0, a: 1}).hint({"$**": "columnstore"}).explain();
assert(planHasStage(db, explain, "COLUMN_SCAN"),
       `Hint should have overridden heuristics to use column scan: ${tojson(explain)}`);
})();
