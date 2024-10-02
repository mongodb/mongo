/**
 * Tests compound wildcard index with unbounded scans including multikey metadata entries doesn't
 * cause any errors.
 *
 * @tags: [
 *   requires_fcv_70,
 *   # explain does not support majority read concern
 *   assumes_read_concern_local,
 * ]
 */
import {getPlanStages, getWinningPlan} from "jstests/libs/analyze_plan.js";

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
    assert.eq({a: ["[MinKey, MaxKey]"], $_path: ["[MinKey, MinKey]", "[\"\", {})"]},
              ixscan.indexBounds,
              explain);
});

const assertNoIndexCorruption = (executionStats) => {
    if (typeof executionStats === 'object') {
        if ("executionSuccess" in executionStats) {
            // The execution should succeed rather than spot any index corruption.
            assert.eq(true, executionStats.executionSuccess, explain);
        }
        assert.eq(executionStats.nReturned, 1, executionStats);
    } else if (Array.isArray(executionStats)) {
        executionStats.forEach(stats => assertNoIndexCorruption(stats));
    }
};
assertNoIndexCorruption(explain.executionStats);
