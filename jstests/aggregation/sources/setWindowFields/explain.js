/**
 * Tests that $setWindowFields stage reports memory footprint per function when explain is run
 * with verbosities "executionStats" and "allPlansExecution". Also tests that the explain output
 * includes a metric for peak memory usage across the entire stage, including each individual
 * function as well as any other internal state.
 *
 * @tags: [assumes_against_mongod_not_mongos]
 */
import {getAggPlanStages} from "jstests/libs/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();
const bigStr = Array(1025).toString();  // 1KB of ','
const nDocs = 1000;
const nPartitions = 50;
// Size was found through logging in 'SpillableCache' class.
const docSize = 1292;

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({_id: i, key: i % nPartitions, bigStr: bigStr});
}
assert.commandWorked(bulk.execute());

/**
 * Checks that the execution stats in the explain output for a $setWindowFields stage are as
 * expected.
 * - 'stages' is an array of the explain output of $setWindowFields stages.
 * - 'expectedFunctionMemUsages' is used to check the memory footprint stats for each function.
 * - 'expectedTotalMemUsage' is used to check the peak memory footprint for the entire stage.
 * - 'verbosity' indicates the explain verbosity used.
 */
function checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotalMemUsage, verbosity) {
    const stages =
        getAggPlanStages(coll.explain(verbosity).aggregate(pipeline), "$_internalSetWindowFields");
    for (let stage of stages) {
        assert(stage.hasOwnProperty("$_internalSetWindowFields"), stage);

        if (verbosity === "executionStats" || verbosity === "allPlansExecution") {
            assert(stage.hasOwnProperty("maxFunctionMemoryUsageBytes"), stage);
            const maxFunctionMemUsages = stage["maxFunctionMemoryUsageBytes"];
            for (let field of Object.keys(maxFunctionMemUsages)) {
                // Ensures that the expected functions are all included and the corresponding
                // memory usage is in a reasonable range.
                if (expectedFunctionMemUsages.hasOwnProperty(field)) {
                    // The estimates being passed in are as close as possible to accurate. Leave 10%
                    // wiggle room for variants that use different amounts of memory.
                    assert.gte(maxFunctionMemUsages[field],
                               expectedFunctionMemUsages[field] * .9,
                               "mismatch for function '" + field + "': " + tojson(stage));
                    assert.lt(maxFunctionMemUsages[field],
                              2.2 * expectedFunctionMemUsages[field],
                              "mismatch for function '" + field + "': " + tojson(stage));
                }
            }
            assert.gt(stage["maxTotalMemoryUsageBytes"],
                      expectedTotalMemUsage * .9,
                      "Incorrect total mem usage: " + tojson(stage));
            assert.lt(stage["maxTotalMemoryUsageBytes"],
                      2.2 * expectedTotalMemUsage,
                      "Incorrect total mem usage: " + tojson(stage));
        } else {
            assert(!stage.hasOwnProperty("maxFunctionMemoryUsageBytes"), stage);
            assert(!stage.hasOwnProperty("maxTotalMemoryUsageBytes"), stage);
        }
    }
}

(function testQueryPlannerVerbosity() {
    const pipeline = [
        {
            $setWindowFields:
                {output: {count: {$sum: 1}, push: {$push: "$bigStr"}, set: {$addToSet: "$bigStr"}}}
        },
    ];
    const stages = getAggPlanStages(coll.explain("queryPlanner").aggregate(pipeline),
                                    "$_internalSetWindowFields");
    checkExplainResult(stages, {}, 0, "queryPlanner");
})();

(function testUnboundedMemUsage() {
    let pipeline = [
        {
            $setWindowFields:
                {output: {count: {$sum: 1}, push: {$push: "$bigStr"}, set: {$addToSet: "$bigStr"}}}
        },
    ];

    // The $setWindowFields stage "streams" one partition at a time, so there's only one instance of
    // each function. For the default [unbounded, unbounded] window type, each function uses memory
    // usage comparable to it's $group counterpart.
    let expectedFunctionMemUsages = {
        count: 72,
        push: nDocs * 1024,
        set: 1024,
    };

    // The total mem usage for unbounded windows is the total from each function as well as the size
    // of all documents in the partition.
    let expectedTotal = nDocs * docSize;
    for (let func in expectedFunctionMemUsages) {
        expectedTotal += expectedFunctionMemUsages[func];
    }

    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "executionStats");
    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "allPlansExecution");

    // Test that the memory footprint is reduced with partitioning.
    pipeline = [
        {
            $setWindowFields: {
                partitionBy: "$key",
                output: {count: {$sum: 1}, push: {$push: "$bigStr"}, set: {$addToSet: "$bigStr"}}
            }
        },
    ];
    expectedFunctionMemUsages = {
        count: 72,
        push: (nDocs / nPartitions) * 1056 + 56,  // 56 constant state size. Uses 1056 per document.
        set: 1144,                                // 1024 for the string, rest is constant state.
    };
    // Add one document for the NextPartitionState structure.
    expectedTotal = ((nDocs / nPartitions) + 1) * docSize;
    for (let func in expectedFunctionMemUsages) {
        expectedTotal += expectedFunctionMemUsages[func];
    }

    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "executionStats");
    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "allPlansExecution");
})();

(function testSlidingWindowMemUsage() {
    const windowSize = 10;
    // The partition iterator will only hold five documents at once. After they are added to the
    // removable document executor they will be released.
    let pipeline = [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {runningSum: {$sum: "$_id", window: {documents: [0, 9]}}}
            }
        },
    ];
    const expectedFunctionMemUsages = {
        // 10 integer values per window, with some extra to account for the $sum accumulator and
        // executor state.
        runningSum: windowSize * 16 + 120,
    };

    let expectedTotal = windowSize * docSize;
    for (let func in expectedFunctionMemUsages) {
        expectedTotal += expectedFunctionMemUsages[func];
    }
    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "executionStats");
    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "allPlansExecution");

    // Test that a window which also looks behind is able to release documents that are no longer
    // needed, thus reducing the total memory footprint. In this example, only half of the window
    // will be in memory at any point in time.
    pipeline = [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {runningSum: {$sum: "$_id", window: {documents: [-5, 4]}}}
            }
        },
    ];
    expectedTotal = (windowSize / 2) * docSize;
    for (let func in expectedFunctionMemUsages) {
        expectedTotal += expectedFunctionMemUsages[func];
    }

    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "executionStats");
    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "allPlansExecution");

    // Adding partitioning doesn't change the peak memory usage.
    pipeline = [
        {
            $setWindowFields: {
                partitionBy: "$key",
                sortBy: {_id: 1},
                output: {runningSum: {$sum: "$_id", window: {documents: [-5, 4]}}}
            }
        },
    ];

    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "executionStats");
    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "allPlansExecution");
})();

(function testRangeBasedWindowMemUsage() {
    const maxDocsInWindow = 20;
    let pipeline = [
        {
            $setWindowFields: {
                sortBy: {_id: 1},
                output: {pushArray: {$push: "$bigStr", window: {range: [-10, 9]}}}
            }
        },
    ];
    // The memory usage is doubled since both the executor and the function state have copies of the
    // large string.
    const expectedFunctionMemUsages = {pushArray: 1024 * maxDocsInWindow * 2};

    let expectedTotal = maxDocsInWindow * docSize;
    for (let func in expectedFunctionMemUsages) {
        expectedTotal += expectedFunctionMemUsages[func];
    }

    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "executionStats");
    checkExplainResult(pipeline, expectedFunctionMemUsages, expectedTotal, "allPlansExecution");
})();
