/**
 * Confirm that we generate EOF plan for $lookup with SBE engine if the main collection does not
 * exist.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   does_not_support_stepdowns,
 * ]
 */

import {aggPlanHasStage, getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {checkSbeCompletelyDisabled} from "jstests/libs/query/sbe_util.js";

if (checkSbeCompletelyDisabled(db)) {
    jsTestLog("Skipping test because SBE is disabled: no lowering of $lookup to SBE.");
    quit();
}

const localColl = db.getCollection("local");
const foreignColl = db.getCollection("foreign");

db.localColl.drop();
db.foreignColl.drop();
assert.commandWorked(db.localColl.insert({_id: 1, a: 5}));
assert.commandWorked(db.foreignColl.insert({_id: 2, b: 5}));

const pipeline =
    [{$lookup: {from: "foreignColl", localField: "a", foreignField: "b", as: "result"}}];
let explain = db.localColl.explain().aggregate(pipeline);
assert(aggPlanHasStage(explain, "EQ_LOOKUP"), explain);

// Remove the main collection and check the EOF plan.
db.localColl.drop();
explain = db.localColl.explain().aggregate(pipeline);
assert(!aggPlanHasStage(explain, "EQ_LOOKUP"), explain);
assert(aggPlanHasStage(explain, "EOF"), explain);
const eofStages = getAggPlanStages(explain, "EOF");
assert.eq(eofStages.length, 1, explain);
assert.eq(eofStages[0].type, "nonExistentNamespace", explain);
