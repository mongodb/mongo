/*
 * Tests the rewrite of queries that contain $and inside $expr.
 * @tags: [
 *   requires_fcv_82
 * ]
 */

import {
    getPlanStages,
    getWinningPlanFromExplain,
    planHasStage
} from "jstests/libs/query/analyze_plan.js";

const coll = db.jstests_expr_and_or_index;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 1, a: 8, b: 3, c: 4, d: 0},
    {_id: 2, a: 1, b: 5, c: 9, d: 1},
    {_id: 3, a: 6, b: 7, c: 2, d: 1},
    {_id: 4, a: 4, b: 8, c: 3, d: 0},
    {_id: 5, a: 9, b: 1, c: 5, d: 1},
    {_id: 6, a: 2, b: 6, c: 7, d: 0},
    {_id: 7, a: 3, b: 4, c: 8, d: 0},
    {_id: 8, a: 5, b: 9, c: 1, d: 0},
    {_id: 9, a: 7, b: 2, c: 6, d: 1},
    {_id: 10, b: 3, c: 4.5, d: 0},
    {_id: 11, a: 8, b: 3.5, d: 0},
    {_id: 12, a: 9, c: 5.5, d: 1},
    {_id: 13, a: 9, b: 1.5, d: 1}
]));

assert.commandWorked(coll.createIndex({a: 1}));

function winningPlanHasStage(explain, stage) {
    const winningPlan = getWinningPlanFromExplain(explain);
    return planHasStage(db, winningPlan, stage);
}

function assertIndexBounds(explain, stage, expectedBounds, testId) {
    const winningPlan = getWinningPlanFromExplain(explain);
    const ixscans = getPlanStages(winningPlan, stage);
    assert.eq(ixscans.length,
              1,
              `Plan is missing ${stage} stage in test ${testId}: ${tojson(winningPlan)}` +
                  tojson(explain));
    if (stage == 'IXSCAN') {
        assert.eq(ixscans[0].indexBounds,
                  expectedBounds,
                  `Expected bounds ${tojson(expectedBounds)} but got ${
                      tojson(ixscans[0].indexBounds)} in test ${testId}` +
                      tojson(explain));
    } else {
        const clusteredIxscanBounds =
            `[${ixscans[0].minRecord.toFixed(1)}, ${ixscans[0].maxRecord.toFixed(1)}]`;
        assert.eq({"_id": [clusteredIxscanBounds]},
                  expectedBounds,
                  `Expected bounds ${tojson(expectedBounds)} but got ${
                      tojson({"_id": [clusteredIxscanBounds]})} in test ${testId}` +
                      tojson(explain));
    }
}

function test({testId, query, expectedStage, expectedBounds = {}}) {
    const explain = coll.find(query).explain();

    if (expectedStage == "IXSCAN") {
        const hasStageIxscan = winningPlanHasStage(explain, "IXSCAN");
        const hasStageClusteredIxscan = winningPlanHasStage(explain, "CLUSTERED_IXSCAN");

        assert(
            hasStageIxscan || hasStageClusteredIxscan,
            `Expected "IXSCAN" or "CLUSTERED_IXSCAN" stage in test ${testId}.` + tojson(explain));

        if (hasStageIxscan) {
            assertIndexBounds(explain, "IXSCAN", expectedBounds, testId);
        } else {
            assertIndexBounds(explain, "CLUSTERED_IXSCAN", expectedBounds, testId);
        }
    } else {
        assert(winningPlanHasStage(explain, expectedStage),
               `Expected ${expectedStage} in test ${testId}.` + tojson(explain));
    }
}

let testCases = [
    {
        testId: 10061700,
        query: {$expr: {$and: [{$eq: ["$_id", 3]}]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"_id": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061701,
        query: {$expr: {$and: [{$eq: ["$_id", 3]}, true]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"_id": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061702,
        query: {$expr: {$and: [{$eq: ["$a", 3]}]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061703,
        query: {$expr: {$and: [{$eq: ["$a", 3]}, true]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061704,
        query: {$expr: {$and: [{$gte: ["$a", 10]}, true, true]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["[10.0, MaxKey]"]}
    },
    {
        testId: 10061705,
        query: {$expr: {$and: [{$gte: ["$_id", 5]}, {$lte: ["$_id", 10]}, true]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"_id": ["[5.0, 10.0]"]}
    },
    {
        testId: 10061706,
        query: {$expr: {$and: [{$lt: ["$_id", 5]}, false, true]}},
        expectedStage: "EOF"
    },
    {
        testId: 10061707,
        query: {$expr: {$or: [{$eq: ["$_id", 3]}, false]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"_id": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061708,
        query: {$expr: {$or: [{$eq: ["$_id", 3]}, false, false]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"_id": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061709,
        query: {$expr: {$or: [{$eq: ["$a", 3]}, false]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061710,
        query: {$expr: {$or: [{$eq: ["$a", 3]}, false, false]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061711,
        query: {$expr: {$or: [{$gte: ["$a", 8]}, {$lt: ["$a", 3]}, false]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["[MinKey, 3.0)", "[8.0, MaxKey]"]}
    },
    {
        testId: 10061712,
        query: {$expr: {$or: [{$lt: ["$_id", 5]}, true]}},
        expectedStage: "COLLSCAN"
    },
    {
        testId: 10061713,
        query: {$expr: {$or: [{$lt: ["$_id", 5]}, false, true]}},
        expectedStage: "COLLSCAN"
    },
    {
        testId: 10061714,
        query: {$expr: {$and: [{$eq: ["$a", 3]}, {$and: [true]}]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["[3.0, 3.0]"]}
    },
    {
        testId: 10061715,
        query: {$expr: {$and: [{$eq: ["$a", 5]}, {$or: [false]}]}},
        expectedStage: "EOF"
    },
    {
        testId: 10061716,
        query: {$expr: {$or: [{$lte: ["$a", 3]}, {$or: [false]}]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["[MinKey, 3.0]"]}
    },
    {
        testId: 10061717,
        query: {$expr: {$or: [{$lte: ["$_id", 3]}, {$and: [true]}]}},
        expectedStage: "COLLSCAN"
    },
    {
        testId: 10061718,
        query: {$expr: {$or: [{$gt: ["$a", 9]}, {$and: [true]}, {$or: [false]}, {$and: [false]}]}},
        expectedStage: "COLLSCAN"
    },
    {
        testId: 10061719,
        query: {$expr: {$and: [{$gt: ["$a", 9]}, {$and: [true]}, {$or: [false]}, {$and: [false]}]}},
        expectedStage: "EOF"
    },
    {
        testId: 10061720,
        query: {$expr: {$and: [{$gt: ["$a", 9]}, {$and: [true]}, {$and: [true]}, {$or: [true]}]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"a": ["(9.0, MaxKey]"]}
    },
    {
        testId: 10061721,
        query: {$expr: {$and: [{$lte: ["$_id", 1]}, {$and: [{$gte: ["$_id", 0]}]}]}},
        expectedStage: "IXSCAN",
        expectedBounds: {"_id": ["[0.0, 1.0]"]}
    }
];

for (let testCase of testCases) {
    test(testCase);
}
