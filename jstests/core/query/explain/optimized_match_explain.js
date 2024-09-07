// @tags: [
//   does_not_support_stepdowns,
// ]

/**
 * Tests that the explain output for $match reflects any optimizations.
 */
import {getAggPlanStage} from "jstests/libs/analyze_plan.js";

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

// Depending on whether the $match can be "pushed down" for SBE, the $match filter may appear in the
// explain plan as a $match pipeline stage or as a MATCH plan stage.
const documentSourceStage = getAggPlanStage(explain, "$match");
const pushedDownFilterStage = getAggPlanStage(explain, "MATCH");
assert(documentSourceStage || pushedDownFilterStage, explain);
if (documentSourceStage) {
    assert(documentSourceStage.hasOwnProperty("$match"), documentSourceStage);
    assert.eq(documentSourceStage["$match"], {c: {$eq: 1}});
} else {
    assert(pushedDownFilterStage.hasOwnProperty("filter"), pushedDownFilterStage);
    assert.eq(pushedDownFilterStage.filter, {c: {$eq: 1}});
}
