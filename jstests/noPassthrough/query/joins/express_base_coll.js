/**
 * This test ensures that the explain output for express queries correctly indicates that join optimization was not applied.
 *  @tags: [
 *   requires_sbe,
 *   requires_fcv_83
 * ]
 */
import {isExpress} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(`${jsTestName()}_db`);
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {_id: 0, a: 1, b: "foo"},
        {_id: 1, a: 1, b: "bar"},
        {_id: 2, a: 2, b: "bar"},
    ]),
);

const foreignColl1 = db[jsTestName() + "_foreign1"];
foreignColl1.drop();

assert.commandWorked(
    foreignColl1.insertMany([
        {_id: 0, a: 1, c: "zoo", d: 1},
        {_id: 1, a: 2, c: "blah", d: 2},
        {_id: 2, a: 2, c: "x", d: 3},
    ]),
);

assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

let expressPipeline = [
    {$match: {_id: 0}},
    {$lookup: {from: foreignColl1.getName(), localField: "a", foreignField: "a", as: "x"}},
];

let explain = coll.explain().aggregate(expressPipeline);

assert(isExpress(db, explain), "Expected the query to be express" + tojson(explain));
assert(!joinOptUsed(explain), "Join optimizer was used when it was not expected to: " + tojson(explain));

let hashJoinPipeline = [...expressPipeline, {$unwind: "$x"}];
explain = coll.explain().aggregate(hashJoinPipeline);
assert(joinOptUsed(explain));

MongoRunner.stopMongod(conn);
