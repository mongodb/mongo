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

function resetThresholds() {
    setParameter(db, "internalQueryColumnScanMinAvgDocSizeBytes", 0);
    setParameter(db, "internalQueryColumnScanMinCollectionSizeBytes", 0);
}

function assertColumnScanUsed(shouldUseColumnScan, msg) {
    let explain = coll.find({}, {_id: 0, a: 1}).explain();
    assert(planHasStage(db, explain, "COLUMN_SCAN") == shouldUseColumnScan,
           `${msg} but column scan was${(shouldUseColumnScan ? " not " : " ")}used: ${
               tojson(explain)}`);
}

// Test heuristics on an empty collection.
setParameter(db, "internalQueryColumnScanMinAvgDocSizeBytes", 1);
assertColumnScanUsed(false, "Collection is empty");

resetThresholds();

setParameter(db, "internalQueryColumnScanMinCollectionSizeBytes", 1);
assertColumnScanUsed(false, "Collection is empty");

resetThresholds();

// Now insert data, content doesn't matter for this test.
for (let i = 0; i < 20; ++i) {
    coll.insert([{x: i}]);
}

// Test min average document size threshold.
setParameter(db, "internalQueryColumnScanMinAvgDocSizeBytes", 1024 * 1024);
assertColumnScanUsed(false, "Collection has documents that are too small to use column scan");

setParameter(db, "internalQueryColumnScanMinAvgDocSizeBytes", 1);
assertColumnScanUsed(true, "Collection has documents large enough to use column scan");

// Test min collection size threshold.
resetThresholds();

setParameter(db, "internalQueryColumnScanMinCollectionSizeBytes", 1024 * 1024);
assertColumnScanUsed(false, "Collection is too small to use column scan");

setParameter(db, "internalQueryColumnScanMinCollectionSizeBytes", 1);
assertColumnScanUsed(true, "Collection is large enough to use column scan");

// Use available memory as the threshold.
setParameter(db, "internalQueryColumnScanMinCollectionSizeBytes", -1);
assertColumnScanUsed(false, "Collection is too small to use column scan");

// Test with both thresholds enabled.
setParameter(db, "internalQueryColumnScanMinCollectionSizeBytes", 1);
setParameter(db, "internalQueryColumnScanMinAvgDocSizeBytes", 1);
assertColumnScanUsed(true, "Collection can use column scan");

setParameter(db, "internalQueryColumnScanMinAvgDocSizeBytes", 1024 * 1024);
assertColumnScanUsed(false, "Collection has documents that are too small to use column scan");

setParameter(db, "internalQueryColumnScanMinAvgDocSizeBytes", 1);
setParameter(db, "internalQueryColumnScanMinCollectionSizeBytes", 1024 * 1024);
assertColumnScanUsed(false, "Collection is too small to use column scan");
})();
