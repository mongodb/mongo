// Validate that generic index scan is used in SBE once max limit for statically generated intervals
// is reached.

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For explain helpers.

// Note that the "getParameter" command is expected to fail in versions of mongod that do not yet
// include the slot-based execution engine. When that happens, however, 'isSBEEnabled' still
// correctly evaluates to false.
const isSBEEnabled = (() => {
    const getParam = db.adminCommand({getParameter: 1, featureFlagSBE: 1});
    return getParam.hasOwnProperty("featureFlagSBE") && getParam.featureFlagSBE.value;
})();
if (!isSBEEnabled) {
    // This test is only relevant when SBE is enabled.
    return;
}

const coll = db.index_bounds_static_limit;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1, d: 1, e: 1}));

// Save the old limit so it can be restored once the tests completes.
const staticLimit = db.adminCommand({
                          getParameter: 1,
                          internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals: 1
                      }).internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals;

const setStaticLimit = function(limit) {
    return db.adminCommand(
        {setParameter: 1, internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals: limit});
};

try {
    // Verify that when the number of statically generated single interval bounds is less than the
    // static limit, the optimized plan is used.
    const optimized =
        coll.find({a: {$in: [1, 2, 3]}, b: {$in: [10, 11, 12]}, c: {$in: [42]}, d: {$lt: 3}})
            .explain("executionStats")
            .executionStats.executionStages;
    assert(planHasStage(db, optimized, "ixseek"), optimized);
    assert(!planHasStage(db, optimized, "chkbounds"), optimized);

    // Verify that when the number of statically generated single interval bounds is greater than
    // the static limit, the generic plan is used.
    setStaticLimit(2);
    const generic =
        coll.find({a: {$in: [1, 2, 3]}, b: {$in: [10, 11, 12]}, c: {$in: [42]}, d: {$lt: 3}})
            .explain("executionStats")
            .executionStats.executionStages;
    assert(planHasStage(db, generic, "chkbounds"), generic);
    assert(planHasStage(db, generic, "ixseek"), generic);
} finally {
    setStaticLimit(staticLimit);
}
})();
