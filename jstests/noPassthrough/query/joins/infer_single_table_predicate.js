/**
 * Runs explain on join queries with single table predicates to ensure the memory of the new filters
 * created by JOO's STP propagation logic is held onto for the whole lifetime of the query.
 * @tags: [
 * requires_fcv_90,
 * requires_sbe,
 * featureFlagPathArrayness
 * ]
 */

import {
    getWinningPlanFromExplain,
    getAllPlanStages,
    getQueryPlanner,
    getRejectedPlans,
} from "jstests/libs/query/analyze_plan.js";
import {runTestWithUnorderedComparison} from "jstests/libs/query/join_utils.js";

// TODO SERVER-127575: remove the feature flag once the flag is defaulted to true.
let conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        internalEnableJoinOptimization: true,
        internalInferSingleTablePredicates: true,
    },
});

const db = conn.getDB("test");

const collA = db[jsTestName() + "A"];
const collB = db[jsTestName() + "B"];
const collC = db[jsTestName() + "C"];
const collD = db[jsTestName() + "C"];

collA.drop();
collB.drop();
collC.drop();

let docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({
        _id: i,
        a: i,
        b: i,
        c: i,
        d: i,
        x: i,
        y: i,
        str_1: (i + 4).toString(),
        str_2: (i + 4).toString(),
    });
}
assert.commandWorked(collA.insertMany(docs));
assert.commandWorked(collB.insertMany(docs));
assert.commandWorked(collC.insertMany(docs));

assert.commandWorked(collA.createIndex({dummy: 1, a: 1, b: 1, c: 1, d: 1, x: 1, y: 1, str_1: 1, str_2: 1}));
assert.commandWorked(collB.createIndex({dummy: 1, a: 1, b: 1, c: 1, d: 1, x: 1, y: 1, str_1: 1, str_2: 1}));
assert.commandWorked(collC.createIndex({dummy: 1, a: 1, b: 1, c: 1, d: 1, x: 1, y: 1, str_1: 1, str_2: 1}));

function extractFilters(plan) {
    const filters = {};

    function traverse(node) {
        if (!node || typeof node !== "object") return;

        if (node.filter !== undefined && node.nss) {
            filters[node.nss.slice(-1)] = node.filter;
        }

        if (Array.isArray(node.inputStages)) {
            for (const child of node.inputStages) {
                traverse(child);
            }
        }
        if (node.inputStage) {
            traverse(node.inputStage);
        }
    }

    traverse(plan);
    return filters;
}

function runTest({pipeline, expectedFilters, expectedResults}) {
    const explain = collA.explain().aggregate(pipeline);

    const queryPlanner = getQueryPlanner(explain);

    const usedJoinOptimization = queryPlanner.winningPlan.hasOwnProperty("usedJoinOptimization")
        ? queryPlanner.winningPlan.usedJoinOptimization
        : false;
    assert(usedJoinOptimization, "Join optimizer was not used as expected: " + tojson(explain));

    const winningPlan = getWinningPlanFromExplain(explain);
    const filters = extractFilters(winningPlan);

    assert.sameMembers(Object.keys(filters), Object.keys(expectedFilters));
    for (const key of Object.keys(expectedFilters)) {
        assert.eq(filters[key], expectedFilters[key]);
    }

    // Verify results are the same with joinOpt off and on.
    runTestWithUnorderedComparison({
        db,
        description: "infer single table predicates",
        coll: collA,
        pipeline,
        expectedResults,
        expectedUsedJoinOptimization: true,
    });
}

// Simplest STP example: for join A.a = B.a and STP B.a = 3, we can infer access path A.a = 3
runTest({
    pipeline: [
        {
            $lookup: {
                from: collB.getName(),
                localField: "a",
                foreignField: "a",
                pipeline: [{$match: {a: 3}}],
                as: "joinedB",
            },
        },
        {$unwind: "$joinedB"},
    ],
    expectedFilters: {
        A: {
            "a": {
                "$eq": 3,
            },
        },
        B: {
            "a": {
                "$eq": 3,
            },
        },
    },
    expectedResults: [
        {
            "_id": 3,
            "a": 3,
            "b": 3,
            "c": 3,
            "d": 3,
            "x": 3,
            "y": 3,
            "str_1": "7",
            "str_2": "7",
            "joinedB": {
                "_id": 3,
                "a": 3,
                "b": 3,
                "c": 3,
                "d": 3,
                "x": 3,
                "y": 3,
                "str_1": "7",
                "str_2": "7",
            },
        },
    ],
});

// Simplest $or case: for join A.a = B.b and STP B.b = 7 or 8, we can infer A.a = 7 or 8.
runTest({
    pipeline: [
        {
            $lookup: {
                from: collB.getName(),
                localField: "a",
                foreignField: "b",
                pipeline: [
                    {
                        $match: {
                            $expr: {
                                $or: [{$eq: ["$b", 7]}, {$eq: ["$b", 8]}],
                            },
                        },
                    },
                ],
                as: "joinedB",
            },
        },
        {$unwind: "$joinedB"},
    ],
    expectedFilters: {
        A: {
            "$and": [
                {
                    "$expr": {
                        "$or": [
                            {
                                "$eq": [
                                    "$a",
                                    {
                                        "$const": 7,
                                    },
                                ],
                            },
                            {
                                "$eq": [
                                    "$a",
                                    {
                                        "$const": 8,
                                    },
                                ],
                            },
                        ],
                    },
                },
            ],
        },
        B: {
            "$and": [
                {
                    "$expr": {
                        "$or": [
                            {
                                "$eq": [
                                    "$b",
                                    {
                                        "$const": 7,
                                    },
                                ],
                            },
                            {
                                "$eq": [
                                    "$b",
                                    {
                                        "$const": 8,
                                    },
                                ],
                            },
                        ],
                    },
                },
            ],
        },
    },
    expectedResults: [
        {
            "_id": 7,
            "a": 7,
            "b": 7,
            "c": 7,
            "d": 7,
            "x": 7,
            "y": 7,
            "str_1": "11",
            "str_2": "11",
            "joinedB": {
                "_id": 7,
                "a": 7,
                "b": 7,
                "c": 7,
                "d": 7,
                "x": 7,
                "y": 7,
                "str_1": "11",
                "str_2": "11",
            },
        },
        {
            "_id": 8,
            "a": 8,
            "b": 8,
            "c": 8,
            "d": 8,
            "x": 8,
            "y": 8,
            "str_1": "12",
            "str_2": "12",
            "joinedB": {
                "_id": 8,
                "a": 8,
                "b": 8,
                "c": 8,
                "d": 8,
                "x": 8,
                "y": 8,
                "str_1": "12",
                "str_2": "12",
            },
        },
    ],
});

// Simplest $and case: join A.a = B.b and A.str_1 = B.str_2 where B.b = 5 and B.str_2 = 9.
// We can infer A.a = 5 and A.str_1 = 9.
runTest({
    pipeline: [
        {
            $lookup: {
                from: collB.getName(),
                let: {a_val: "$a", str1_val: "$str_1"},
                pipeline: [
                    {
                        $match: {
                            $and: [
                                {b: 5},
                                {str_2: "9"},
                                {
                                    $expr: {
                                        $and: [{$eq: ["$b", "$$a_val"]}, {$eq: ["$str_2", "$$str1_val"]}],
                                    },
                                },
                            ],
                        },
                    },
                ],
                as: "joinedB",
            },
        },
        {$unwind: "$joinedB"},
    ],
    expectedFilters: {
        A: {
            "$and": [
                {
                    "a": {
                        "$eq": 5,
                    },
                },
                {
                    "str_1": {
                        "$eq": "9",
                    },
                },
            ],
        },
        B: {
            "$and": [
                {
                    "b": {
                        "$eq": 5,
                    },
                },
                {
                    "str_2": {
                        "$eq": "9",
                    },
                },
            ],
        },
    },
    expectedResults: [
        {
            "_id": 5,
            "a": 5,
            "b": 5,
            "c": 5,
            "d": 5,
            "x": 5,
            "y": 5,
            "str_1": "9",
            "str_2": "9",
            "joinedB": {
                "_id": 5,
                "a": 5,
                "b": 5,
                "c": 5,
                "d": 5,
                "x": 5,
                "y": 5,
                "str_1": "9",
                "str_2": "9",
            },
        },
    ],
});

// Let's make it a little harder!
// For joins A.a = B.a, A.str_1 = B.str_1 and C.a = B.a and STP B.a = 3 and B.str_1 = "7", we
// can infer A.a = 3, A.str_1 = "7" and C.a = 3.
runTest({
    pipeline: [
        {
            $lookup: {
                from: collB.getName(),
                as: "joined",
                let: {a_val: "$a", str1_val: "$str_1"},
                pipeline: [
                    {
                        $match: {
                            $expr: {
                                $and: [
                                    {$eq: ["$a", "$$a_val"]},
                                    {$eq: ["$str_1", "$$str1_val"]},
                                    {$eq: ["$a", 3]},
                                    {$eq: ["$str_1", "7"]},
                                ],
                            },
                        },
                    },
                ],
            },
        },
        {$unwind: "$joined"},
        {
            $lookup: {
                from: collC.getName(),
                localField: "joined.a",
                foreignField: "a",
                as: "final",
            },
        },
        {$unwind: "$final"},
    ],
    expectedFilters: {
        A: {
            "$and": [
                {
                    "$expr": {
                        "$eq": [
                            "$a",
                            {
                                "$const": 3,
                            },
                        ],
                    },
                },
                {
                    "$expr": {
                        "$eq": [
                            "$str_1",
                            {
                                "$const": "7",
                            },
                        ],
                    },
                },
            ],
        },
        B: {
            "$and": [
                {
                    "$expr": {
                        "$and": [
                            {
                                "$eq": [
                                    "$a",
                                    {
                                        "$const": 3,
                                    },
                                ],
                            },
                            {
                                "$eq": [
                                    "$str_1",
                                    {
                                        "$const": "7",
                                    },
                                ],
                            },
                        ],
                    },
                },
            ],
        },
        C: {
            "$and": [
                {
                    "$expr": {
                        "$eq": [
                            "$a",
                            {
                                "$const": 3,
                            },
                        ],
                    },
                },
            ],
        },
    },
    expectedResults: [
        {
            "_id": 3,
            "a": 3,
            "b": 3,
            "c": 3,
            "d": 3,
            "x": 3,
            "y": 3,
            "str_1": "7",
            "str_2": "7",
            "joined": {
                "_id": 3,
                "a": 3,
                "b": 3,
                "c": 3,
                "d": 3,
                "x": 3,
                "y": 3,
                "str_1": "7",
                "str_2": "7",
            },
            "final": {
                "_id": 3,
                "a": 3,
                "b": 3,
                "c": 3,
                "d": 3,
                "x": 3,
                "y": 3,
                "str_1": "7",
                "str_2": "7",
            },
        },
    ],
});

MongoRunner.stopMongod(conn);
