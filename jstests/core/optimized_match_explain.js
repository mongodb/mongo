// @tags: [
//   does_not_support_stepdowns,
// ]

/**
 * Tests that the explain output for $match reflects any optimizations.
 */
import {getAggPlanStage} from "jstests/libs/analyze_plan.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";

// TODO SERVER-72549: Remove 'featureFlagSbeFull' used by SBE Pushdown feature here and below.
const featureFlagSbeFull = checkSBEEnabled(db, ["featureFlagSbeFull"]);

const coll = db.match_explain;
coll.drop();

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 2, b: 3}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: 4}));

// Explain output should reflect optimizations.
// $and should not be in the explain output because it is optimized out.
let explain = coll.explain().aggregate(
    [{$sort: {b: -1}}, {$addFields: {c: {$mod: ["$a", 4]}}}, {$match: {$and: [{c: 1}]}}]);

assert.commandWorked(explain);
if (featureFlagSbeFull) {
    assert.eq(getAggPlanStage(explain, "$match"), null);
} else {
    assert.eq(getAggPlanStage(explain, "$match"), {$match: {c: {$eq: 1}}});
}