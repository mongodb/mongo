/**
 * Test that when $setWindowFields requires metadata, it does not take the express path
 * even if the desugared query would otherwise be eligible.
 * @tags: [requires_fcv_82]
 */
import {getQueryPlanner, planHasStage} from "jstests/libs/query/analyze_plan.js";
const coll = db[jsTestName()];
coll.drop();
const documents = [{
    _id: 31,
    date: 2,
}];

assert.commandWorked(coll.insert(documents));

// We need a sort key, but the point _id query would normally take the express path.
let q = [
    {"$match": {"_id": 31}},
    {
        "$setWindowFields":
            {"sortBy": {"date": 1}, "partitionBy": "$array", "output": {"x": {"$denseRank": {}}}}
    }
];

// This query should return one successfully ranked document.
let result = coll.aggregate(q).toArray();

assert.eq(result, [{_id: 31, date: 2, x: 1}]);

let explain = coll.explain().aggregate(q);
let planner = getQueryPlanner(explain);
assert(planHasStage(db, planner.winningPlan, "SORT_KEY_GENERATOR"), planner);
