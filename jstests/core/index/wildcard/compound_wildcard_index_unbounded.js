/**
 * Tests compound wildcard index with unbounded scans including multikey metadata entries doesn't
 * cause any errors.
 *
 * @tags: [
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const coll = db.compound_wildcard_index_unbounded;
coll.drop();
const keyPattern = {
    a: 1,
    "$**": 1
};
const keyProjection = {
    wildcardProjection: {a: 0}
};
assert.commandWorked(coll.createIndex(keyPattern, keyProjection));
assert.commandWorked(coll.insert({a: 1, b: 1}));
// Add an array field in order to make wildcard insert a multikey path metadata entry.
assert.commandWorked(coll.insert({b: [1, 2]}));

const query = {
    a: {$exists: true}
};
const explain = coll.find(query).hint(keyPattern).explain('executionStats');
const plan = getWinningPlan(explain.queryPlanner);
const ixscans = getPlanStages(plan, "IXSCAN");
// Asserting that we have unbounded index scans on $_path so that multikey metadata will also be
// included in the scan.
assert.gt(ixscans.length, 0, explain);
ixscans.forEach(ixscan => {
    assert.eq({a: 1, $_path: 1}, ixscan.keyPattern, explain);
    assert.eq({a: ["[MinKey, MaxKey]"], $_path: ["[MinKey, MaxKey]"]}, ixscan.indexBounds, explain);
});

// TODO SERVER-78307: Fix the erroneous index corruption result.
const assertIndexCorruption = (executionStats) => {
    if (typeof executionStats === 'object') {
        if ("executionSuccess" in executionStats) {
            assert.eq(false, executionStats.executionSuccess, explain);
            assert.eq(ErrorCodes.DataCorruptionDetected, executionStats.errorCode, explain);
        }
    } else if (Array.isArray(executionStats)) {
        executionStats.forEach(stats => assertIndexCorruption(stats));
    }
};
assertIndexCorruption(explain.executionStats);
})();
