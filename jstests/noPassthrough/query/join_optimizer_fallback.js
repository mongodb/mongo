/**
 * Tests that join optimization falls back gracefully in cases when it is not supported.
 * @tags: [
 *   requires_fcv_83,
 * ]
 */
import {getWinningPlanFromExplain, getAllPlanStages} from "jstests/libs/query/analyze_plan.js";

function joinOptimizedUsed(explain) {
    const stages = getAllPlanStages(getWinningPlanFromExplain(explain)).map((stage) => stage.stage);
    const joinOptimzierStages = [
        "NESTED_LOOP_JOIN_EMBEDDING",
        "HASH_JOIN_EMBEDDING",
        "INDEXED_NESTED_LOOP_JOIN_EMBEDDING",
    ];
    return stages.some((stage) => joinOptimzierStages.includes(stage));
}

let conn = MongoRunner.runMongod();

// Test that cross-DB joins are not accepted by the join optimizer.
const db1 = "test";
const db2 = "test2";
const db3 = "test3";

const coll1 = conn.getDB(db1)[jsTestName() + "_coll1"];
const coll12 = conn.getDB(db1)[jsTestName() + "_coll2"];
const coll2 = conn.getDB(db2)[jsTestName() + "_coll2"];
const coll3 = conn.getDB(db3)[jsTestName() + "_coll3"];

coll1.drop();
coll12.drop();
coll2.drop();
coll3.drop();

assert.commandWorked(coll1.insertOne({a: 1}));
assert.commandWorked(coll12.insertOne({a: 1}));
assert.commandWorked(coll2.insertOne({a: 1}));
assert.commandWorked(coll3.insertOne({a: 1}));

function runTestCase({pipeline, expectedCount, expectedJoinOptimizer}) {
    assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
    assert.eq(coll1.aggregate(pipeline).toArray().length, expectedCount);
    assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

    const explain = coll1.explain().aggregate(pipeline);
    assert.eq(
        expectedJoinOptimizer,
        joinOptimizedUsed(explain),
        "Expected join optimizer and actual usage differ: " + tojson(explain),
    );
}

runTestCase({
    pipeline: [
        {
            $lookup: {
                from: {
                    db: db2,
                    coll: coll2.getName(),
                },
                localField: "a",
                foreignField: "a",
                as: "coll2",
            },
        },
        {$unwind: "$coll2"},
        {
            $lookup: {
                from: {
                    db: db3,
                    coll: coll3.getName(),
                },
                localField: "a",
                foreignField: "a",
                as: "coll3",
            },
        },
        {$unwind: "$coll3"},
    ],
    expectedCount: 1,
    expectedJoinOptimizer: false,
});

// Prefix eligible and suffix is cross-DB $lookup
runTestCase({
    pipeline: [
        {
            $lookup: {
                from: {
                    db: db1,
                    coll: coll12.getName(),
                },
                localField: "a",
                foreignField: "a",
                as: "coll2",
            },
        },
        {$unwind: "$coll2"},
        {
            $lookup: {
                from: {
                    db: db3,
                    coll: coll3.getName(),
                },
                localField: "a",
                foreignField: "a",
                as: "coll3",
            },
        },
        {$unwind: "$coll3"},
    ],
    expectedCount: 1,
    expectedJoinOptimizer: false,
});

MongoRunner.stopMongod(conn);
