/**
 * Tests that join-opt executor code successfully pushes down SBE-eligible suffix stages.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    aggPlanHasStage,
    getWinningPlanFromExplain,
    getAllPlanStages,
} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const base = db[jsTestName() + "_base"];
const collA = db[jsTestName() + "_a"];
const collB = db[jsTestName() + "_b"];

function resetData() {
    assert(base.drop());
    assert(collA.drop());
    assert(collB.drop());

    assert.commandWorked(
        base.insertMany([
            {_id: 1, a: 1, b: 10, x: 2},
            {_id: 2, a: 1, b: 20, x: 1},
            {_id: 3, a: 2, b: 30, x: 3},
        ]),
    );

    assert.commandWorked(
        collA.insertMany([
            {_id: 101, a: 1, tag: "left"},
            {_id: 102, a: 2, tag: "right"},
        ]),
    );

    assert.commandWorked(
        collB.insertMany([
            {_id: 201, b: 10, rank: 2},
            {_id: 202, b: 20, rank: 1},
            {_id: 203, b: 30, rank: 3},
        ]),
    );

    // Join indexes.
    assert.commandWorked(base.createIndex({a: 1}));
    assert.commandWorked(base.createIndex({b: 1}));
    assert.commandWorked(collA.createIndex({a: 1}));
    assert.commandWorked(collB.createIndex({b: 1}));

    // Path-arrayness metadata indexes.
    assert.commandWorked(base.createIndex({dummy: 1, a: 1, b: 1, x: 1}));
    assert.commandWorked(collA.createIndex({dummy: 1, a: 1, tag: 1}));
    assert.commandWorked(collB.createIndex({dummy: 1, b: 1, rank: 1}));
}

function setJoinOpt(enabled) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalEnableJoinOptimization: enabled}),
    );
}

function runAndCompare({pipeline, suffixStagesPushedDown = []}) {
    setJoinOpt(false);
    const expected = base.aggregate(pipeline).toArray();

    setJoinOpt(true);
    const actual = base.aggregate(pipeline).toArray();
    assertArrayEq({actual, expected});

    const explain = base.explain().aggregate(pipeline);
    assert(joinOptUsed(explain), "Expected join optimization to be used: " + tojson(explain));

    const loweredStageToPlanStages = {
        "$match": ["MATCH"],
        "$group": ["GROUP"],
        "$sort": ["SORT"],
        "$limit": ["LIMIT"],
        "$skip": ["SKIP"],
        "$project": ["PROJECTION_DEFAULT", "PROJECTION_SIMPLE", "PROJECTION_COVERED"],
        "$set": ["PROJECTION_DEFAULT", "PROJECTION_SIMPLE", "PROJECTION_COVERED"],
    };
    const queryPlan = getWinningPlanFromExplain(explain);
    const planStages = getAllPlanStages(queryPlan).map((s) => s.stage);

    for (const stageName of suffixStagesPushedDown) {
        assert(
            !aggPlanHasStage(explain, stageName),
            `Expected ${stageName} to be pushed down to SBE but found it in classic: ${tojson(explain)}`,
        );

        const expectedPlanStages = loweredStageToPlanStages[stageName];
        assert(expectedPlanStages, `No plan-stage mapping for ${stageName}`);

        assert(
            expectedPlanStages.some((s) => planStages.includes(s)),
            `Expected one of ${tojson(expectedPlanStages)} in winning plan SBE stages ${tojson(planStages)}. Full plan: ${tojson(queryPlan)}`,
        );
    }
}

const originalQFC = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
);

try {
    resetData();

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}),
    );

    runAndCompare({
        pipeline: [
            {$lookup: {from: collA.getName(), localField: "a", foreignField: "a", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: collB.getName(), localField: "b", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"},
            {$limit: 2},
        ],
        suffixStagesPushedDown: ["$limit"],
    });

    runAndCompare({
        pipeline: [
            {$lookup: {from: collA.getName(), localField: "a", foreignField: "a", as: "fromA"}},
            {$unwind: "$fromA"},
            {$lookup: {from: collB.getName(), localField: "b", foreignField: "b", as: "fromB"}},
            {$unwind: "$fromB"},
            {$group: {_id: "$fromA.tag", totalRank: {$sum: "$fromB.rank"}, n: {$sum: 1}}},
            {$sort: {_id: 1}},
        ],
        suffixStagesPushedDown: ["$group", "$sort"],
    });

    runAndCompare({
        pipeline: [
            {$lookup: {from: collA.getName(), localField: "a", foreignField: "a", as: "fromA"}},
            {$unwind: "$fromA"},
            {$skip: 1},
            {$project: {a: 1, fromA: 1, extra: "$fromA.tag"}},
            {$set: {extra2: "$extra"}},
            {$group: {_id: "$extra2", totalA: {$sum: "$a"}, n: {$sum: 1}}},
        ],
        suffixStagesPushedDown: ["$skip", "$project", "$set", "$group"],
    });
} finally {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryFrameworkControl: originalQFC.internalQueryFrameworkControl,
        }),
    );
}
