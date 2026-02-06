/**
 * Ensures that the HashJoinStage correctly spills to disk when memory limits are exceeded
 * in join optimizer queries.
 * @tags: [
 *   requires_fcv_83,
 *   requires_getmore,
 *   requires_sbe,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getWinningPlanFromExplain, getExecutionStages, getPlanStages} from "jstests/libs/query/analyze_plan.js";

function getHashJoinStages(explain) {
    const execStages = getExecutionStages(explain);
    assert.gt(execStages.length, 0, `No execution stages found: ${tojson(explain)}`);
    // Look for hash_join stages in the execution tree.
    return getPlanStages(execStages[0], "hj");
}

function joinOptimizerUsedHashJoin(explain) {
    const winningPlan = getWinningPlanFromExplain(explain);
    const stages = getPlanStages(winningPlan, "HASH_JOIN_EMBEDDING");
    return stages.length > 0;
}

function verifySpillingStats(hashJoinStages, shouldSpill) {
    for (const hashJoinStage of hashJoinStages) {
        if (shouldSpill) {
            assert.eq(hashJoinStage.usedDisk, true, `Expected usedDisk=true: ${tojson(hashJoinStage)}`);
            assert.gt(hashJoinStage.spills, 0, `Expected spills > 0: ${tojson(hashJoinStage)}`);
            assert.gt(hashJoinStage.spilledRecords, 0, `Expected spilledRecords > 0: ${tojson(hashJoinStage)}`);
            assert.gt(hashJoinStage.spilledBytes, 0, `Expected spilledBytes > 0: ${tojson(hashJoinStage)}`);
            assert.gt(
                hashJoinStage.spilledDataStorageSize,
                0,
                `Expected spilledDataStorageSize > 0: ${tojson(hashJoinStage)}`,
            );
        } else {
            assert.eq(hashJoinStage.usedDisk, false, `Expected usedDisk=false: ${tojson(hashJoinStage)}`);
            assert.eq(hashJoinStage.spills, 0, `Expected spills = 0: ${tojson(hashJoinStage)}`);
            assert.eq(hashJoinStage.spilledRecords, 0, `Expected spilledRecords = 0: ${tojson(hashJoinStage)}`);
            assert.eq(hashJoinStage.spilledBytes, 0, `Expected spilledBytes = 0: ${tojson(hashJoinStage)}`);
            assert.eq(
                hashJoinStage.spilledDataStorageSize,
                0,
                `Expected spilledDataStorageSize = 0: ${tojson(hashJoinStage)}`,
            );
        }
    }
}

const conn = MongoRunner.runMongod({setParameter: {allowDiskUseByDefault: true}});
const db = conn.getDB(jsTestName());

const coll1 = db[jsTestName() + "_1"];
const coll2 = db[jsTestName() + "_2"];

function dropAndInsertData(coll, numDocs) {
    coll.drop();

    let docs = [];
    for (let i = 0; i < numDocs; i++) {
        // Create documents with some data to increase memory usage.
        docs.push({_id: i, a: i, b: i, data: "x".repeat(100)});
    }
    assert.commandWorked(coll.insertMany(docs));
}

dropAndInsertData(coll1, 100);
dropAndInsertData(coll2, 1000);

// Store the original memory limit to restore later.
const oldMemoryLimit = assert.commandWorked(
    db.adminCommand({
        getParameter: 1,
        internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill: 1,
    }),
).internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill;

// Helper function to run a join and verify spilling behavior.
function testJoinSpilling(coll, lookupPipeline, shouldSpill) {
    // Enable join optimization.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

    const explain = coll.explain("executionStats").aggregate(lookupPipeline);
    assert(
        joinOptimizerUsedHashJoin(explain),
        "Join optimizer with HashJoin was not used as expected: " + tojson(explain),
    );

    const hashJoinStages = getHashJoinStages(explain);
    verifySpillingStats(hashJoinStages, shouldSpill);

    // Verify query returns correct results.
    const results = coll.aggregate(lookupPipeline).toArray();
    assert.eq(results.length, 100, "Expected 100 results from join");

    // Disable join optimization and run again to verify the results.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
    const resultsWithoutJoinOptimization = coll.aggregate(lookupPipeline).toArray();
    assert.eq(resultsWithoutJoinOptimization.length, 100, "Expected 100 results from non-optimized join");

    // Verify results are the same with and without join optimization.
    assertArrayEq({actual: results, expected: resultsWithoutJoinOptimization});
}

// Verify no spilling with default high memory limit.
jsTest.log.info("Test 1: No spilling with default high memory limit");
const lookupPipeline = [
    {
        $lookup: {
            from: coll2.getName(),
            localField: "a",
            foreignField: "a",
            as: "joined",
        },
    },
    {
        $unwind: "$joined",
    },
];

testJoinSpilling(coll1, lookupPipeline, false /* shouldSpill */);

// Set a very low memory limit to force spilling.
const lowMemoryLimit = 1024; // 1KB
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill: lowMemoryLimit,
    }),
);

// Verify spilling occurs with low memory limit.
jsTest.log.info("Test 2: Spilling with low memory limit");
testJoinSpilling(coll1, lookupPipeline, true /* shouldSpill */);

// Test with multiple join predicates causing spilling.
jsTest.log.info("Test 3: Multi-predicate join with spilling");
const multiPredicateLookupPipeline = [
    {
        $lookup: {
            from: coll2.getName(),
            localField: "a",
            foreignField: "a",
            as: "joined",
        },
    },
    {$unwind: "$joined"},
    {$match: {$expr: {$eq: ["$b", "$joined.b"]}}},
];

testJoinSpilling(coll1, multiPredicateLookupPipeline, true /* shouldSpill */);

// Test with multiple lookups causing spilling.
jsTest.log.info("Test 4: Multiple-lookup with spilling");
const multipleLookupPipeline = [
    {
        $lookup: {
            from: coll1.getName(),
            localField: "a",
            foreignField: "a",
            as: "joined1",
        },
    },
    {$unwind: "$joined1"},
    {
        $lookup: {
            from: coll1.getName(),
            localField: "a",
            foreignField: "a",
            as: "joined2",
        },
    },
    {$unwind: "$joined2"},
];
testJoinSpilling(coll1, multipleLookupPipeline, true /* shouldSpill */);

// Self-lookup with spilling.
jsTest.log.info("Test 5: Self-lookup with spilling");
const selfLookupPipeline = [
    {
        $lookup: {
            from: coll1.getName(),
            localField: "a",
            foreignField: "a",
            as: "self",
        },
    },
    {$unwind: "$self"},
];

testJoinSpilling(coll1, selfLookupPipeline, true /* shouldSpill */);

// Restore original memory limit.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashJoinApproxMemoryUseInBytesBeforeSpill: oldMemoryLimit,
    }),
);

MongoRunner.stopMongod(conn);
