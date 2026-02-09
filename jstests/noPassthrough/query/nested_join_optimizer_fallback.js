/**
 * Tests that OPTIMIZE_SUBPIPELINE_FOR_JOIN_OPT is not applied by the rewrite engine when the $lookup subpipeline
 * has a depth greater than 1 (eg it is not a top level subpipeline).
 * @tags: [
 *   requires_fcv_83,
 *   requires_sbe
 * ]
 */
import {getQueryPlanner, getAllPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";

let conn = MongoRunner.runMongod();

const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {_id: 0, a: 1, b: "foo"},
        {_id: 1, a: 1, b: "bar"},
        {_id: 2, a: 2, b: "bar"},
        {_id: 3, a: null, b: "bar"},
        {_id: 4, b: "bar"},
    ]),
);

const foreignColl1 = db[jsTestName() + "_foreign1"];
foreignColl1.drop();

assert.commandWorked(
    foreignColl1.insertMany([
        {_id: 0, a: 1, b: "bar", c: "zoo", d: 1},
        {_id: 1, a: 2, b: "foo", c: "blah", d: 2},
        {_id: 2, a: 2, b: "babs", c: "x", d: 3},
        {_id: 3, a: null, b: "baz", c: "x", d: 4},
        {_id: 4, c: "x", d: 5},
    ]),
);

const foreignColl2 = db[jsTestName() + "_foreign2"];
foreignColl2.drop();
assert.commandWorked(
    foreignColl2.insertMany([
        {_id: 0, b: "bar", c: "blah", d: 2},
        {_id: 1, b: "bar", c: "blah", d: 6},
        {_id: 2, b: "baz", c: "blah", d: 7},
    ]),
);

assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

let pipeline = [
    {
        $lookup: {
            from: foreignColl1.getName(),
            as: "x",
            localField: "a",
            foreignField: "a",
            pipeline: [
                {
                    $lookup: {
                        from: foreignColl2.getName(),
                        localField: "b",
                        foreignField: "b",
                        as: "fromColl2",
                        // This is a nested subpipeline, it has a depth of 2.
                        pipeline: [{$match: {d: {$lt: 3}}}, {$match: {c: "blah"}}, {$match: {_id: {$gte: 0}}}],
                    },
                },
                {
                    $unwind: "$fromColl2",
                },
            ],
        },
    },
    {$unwind: "$x"},
];
const explain = coll.explain().aggregate(pipeline);

assert(!joinOptUsed(explain));
/*
 * Inspect to make sure the nested pipeline was not optimized since it is nested too deep.
 */
const nestedSubPipeline = explain.stages[1]["$lookup"]["pipeline"][0]["$lookup"]["pipeline"];
/* Ensure that the three consecutive $match stages weren't collapsed into one single stage */
assert.eq(nestedSubPipeline.length, 3, nestedSubPipeline);

MongoRunner.stopMongod(conn);
