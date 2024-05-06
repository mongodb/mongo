/**
 * Tests that explain reports the skip stage statistics correctly.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {getPlanStage, getWinningPlan} from "jstests/libs/analyze_plan.js";

const coll = db.explain_skip;
coll.drop();

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: 3}));
assert.commandWorked(coll.insert({a: 2, b: 1}));
assert.commandWorked(coll.insert({a: 2, b: 2}));
assert.commandWorked(coll.insert({a: 2, b: 3}));

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

const explain = coll.find({a: 1, b: 1}).sort({_id: 1}).skip(5).explain();
const winningPlan = getWinningPlan(explain.queryPlanner);
const skipStage = getPlanStage(winningPlan, "SKIP");
assert.neq(null, skipStage, explain);
assert.eq(5, skipStage.skipAmount, explain);
