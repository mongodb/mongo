/**
 * Test that the const user-defined variable will not be const-folded. The SBE plan cache should
 * keep track of the let variable and refresh the let variable when the plan is recovered.
 *
 * The test ensures that two queries with only difference in the const let variable have the same
 * 'planCacheKey' and return correct results.
 *
 * Please see SERVER-81807 for details.
 *
 * @tags: [
 *   featureFlagSbeFull
 * ]
 */

import {getPlanCacheKeyFromExplain} from "jstests/libs/analyze_plan.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.sbe_plan_cache_with_const_let_var;
coll.drop();

assert.commandWorked(coll.insert({}));

let result;
const findWithConstLet5 = {
    find: coll.getName(),
    filter: {},
    projection: {_id: "$$a"},
    limit: 1,
    let : {a: {$add: [2, 3]}},
};
const findWithConstLet500 = {
    find: coll.getName(),
    filter: {},
    projection: {_id: "$$a"},
    limit: 1,
    let : {a: {$add: [200, 300]}},
};

result = assert.commandWorked(db.runCommand(findWithConstLet5)).cursor.firstBatch;
// Test that right result is returned.
assert.eq(result, [{_id: 5}]);
assert.gt(coll.getPlanCache().list().length, 0, coll.getPlanCache().list());

let exp = assert.commandWorked(db.runCommand({explain: findWithConstLet5}))
let planCacheKey = getPlanCacheKeyFromExplain(exp, db);

result = assert.commandWorked(db.runCommand(findWithConstLet500)).cursor.firstBatch;
exp = assert.commandWorked(db.runCommand({explain: findWithConstLet500}))

// Test that two queries with different consts have the same planCacheKey and the returned results
// are correct and different. Otherwise, the const let value may be incorrectly baked into SBE plan
// and then the 2nd query would return the same document as the first query.
assert.eq(result, [{_id: 500}]);
assert.eq(planCacheKey, getPlanCacheKeyFromExplain(exp, db), coll.getPlanCache().list());

MongoRunner.stopMongod(conn);
