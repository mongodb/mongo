/**
 * This test asserts that the logic in the CBR sampling estimator for determining if a document val
 * is within the index bounds is correct when the index intervals are ascending or descending.
 */
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption.
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

assert.commandWorked(
    db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: true, internalQueryCBRCEMode: "samplingCE"}),
);
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 2, b: 2}));
assert.commandWorked(coll.insert({a: 2, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 20, b: 20}));
assert.commandWorked(coll.insert({a: 1, b: null}));
assert.commandWorked(coll.insert({a: null, b: 20}));
assert.commandWorked(coll.insert({b: 20}));
assert.commandWorked(coll.insert({b: 21}));
assert.commandWorked(coll.insert({a: null}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: -1}));

/**
 * These queries verify that ascending and descending index intervals yield the same estimate.
 */
try {
    function runTest(filter, sort, cardinalityEstimate, indexBounds, indexDirection) {
        let winningPlan = coll.find(filter).sort(sort).explain().queryPlanner.winningPlan;
        assert.eq(winningPlan.inputStage.indexBounds, indexBounds);
        assert.eq(winningPlan.inputStage.direction, indexDirection);
        assert.eq(winningPlan.cardinalityEstimate, cardinalityEstimate);
    }
    /**
     * The index is read forwards because the sort predicate specifies ascending order and thus the
     * index interval bounds are ascending.
     */
    runTest({}, {a: 1}, 10, {a: ["[MinKey, MaxKey]"]}, "forward");
    /**
     * The index is read backwards because the sort predicate specifies descending order and thus
     * the index interval bounds are descending.
     */
    runTest({}, {a: -1}, 10, {a: ["[MaxKey, MinKey]"]}, "backward");
    /**
     * The index is defined with descending order so the index is technically read forwards, but
     * the important part is that the index interval bounds are descending.
     */
    runTest({}, {b: -1}, 10, {b: ["[MaxKey, MinKey]"]}, "forward");
    /**
     * The index is read forwards because the sort predicate specifies ascending order and thus the
     * index interval bounds are ascending.
     */
    runTest({a: {$gt: 1, $lte: 2}}, {a: 1}, 2, {a: ["(1.0, 2.0]"]}, "forward");
    /**
     * The index is read backwards because the sort predicate specifies descending order and thus
     * the index interval bounds are descending.
     */
    runTest({a: {$gt: 1, $lte: 2}}, {a: -1}, 2, {a: ["[2.0, 1.0)"]}, "backward");
    /**
     * The index is defined with descending order so the index is technically read forwards, but
     * the important part is that the index interval bounds are descending.
     */
    runTest({b: {$gt: 1, $lte: 2}}, {}, 2, {b: ["[2.0, 1.0)"]}, "forward");
    /**
     * The index is read forwards because the sort predicate specifies ascending order and thus the
     * index interval bounds are ascending.
     */
    runTest({a: {$gt: 1}, b: {$lte: 20}}, {a: 1}, 3, {a: ["(1.0, inf]"]}, "forward");
    /**
     * The index is read backwards because the sort predicate specifies descending order and thus
     * the index interval bounds are descending.
     */
    runTest({a: {$gt: 1}, b: {$lte: 20}}, {a: -1}, 3, {a: ["[inf, 1.0)"]}, "backward");
    /**
     * Test that intervals containing null are estimated correctly.
     */
    runTest({a: {$ne: 1}}, {a: 1}, 7, {a: ["[MinKey, 1.0)", "(1.0, MaxKey]"]}, "forward");
    runTest({a: {$ne: 1}}, {a: -1}, 7, {a: ["[MaxKey, 1.0)", "(1.0, MinKey]"]}, "backward");
    runTest({b: {$ne: 1}}, {b: 1}, 8, {b: ["[MinKey, 1.0)", "(1.0, MaxKey]"]}, "backward");
    runTest({b: {$ne: 1}}, {b: -1}, 8, {b: ["[MaxKey, 1.0)", "(1.0, MinKey]"]}, "forward");
    /**
     * Test that {$exists: false} is estimated correctly.
     */
    runTest({a: {$exists: false}}, {}, 2, {a: ["[null, null]"]}, "forward");
    runTest({b: {$exists: false}}, {}, 1, {b: ["[null, null]"]}, "forward");
} finally {
    /** Ensure that query knob doesn't leak into other testcases in the suite. */
    assert.commandWorked(db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: false}));
}
