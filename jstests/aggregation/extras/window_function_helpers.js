load("jstests/aggregation/extras/utils.js");  // arrayEq
load("jstests/libs/analyze_plan.js");         // For getAggPlanStages().

/**
 * Create a collection of tickers and prices.
 */
function seedWithTickerData(coll, docsPerTicker) {
    for (let i = 0; i < docsPerTicker; i++) {
        assert.commandWorked(
            coll.insert({_id: i, partIndex: i, ticker: "T1", price: (500 - i * 10)}));

        assert.commandWorked(coll.insert(
            {_id: i + docsPerTicker, partIndex: i, ticker: "T2", price: (400 + i * 10)}));
    }
}

function forEachPartitionCase(callback) {
    callback(null);
    callback("$ticker");
    callback({$toLower: "$ticker"});
}

const documentBounds = [
    ["unbounded", 0],
    ["unbounded", -1],
    ["unbounded", 1],
    ["unbounded", 5],
    ["unbounded", "unbounded"],
    [0, "unbounded"],
    [0, 1],
    ["current", 2],
    [0, 5],
    ["current", 4],
    [-1, 1],
    [-2, "unbounded"],
    [1, "unbounded"],
    [-3, -1],
    [1, 3],
    [-2, 3],
];

function forEachDocumentBoundsCombo(callback) {
    documentBounds.forEach(function(bounds, index) {
        let boundsCombo = [bounds];
        for (let j = index + 1; j < documentBounds.length; j++) {
            boundsCombo.push(documentBounds[j]);
            callback(boundsCombo);
            boundsCombo.pop();
        }
    });

    // Add a few combinations that test 3+ bounds.
    callback([["unbounded", "unbounded"], ["unbounded", 0], ["unbounded", -1]]);
    callback([[-1, 1], [-1, 0], ["unbounded", -1]]);
    callback([[2, 3], [2, 3], ["unbounded", -1]]);
    callback([[-5, -5], [-5, -5], [-5, -5]]);
}

/**
 * Runs a pipeline containing a $group that computes the window-function equivalent using the
 * given partition key, accumulator, index in the current partition, and bounds.
 *
 * The bounds are an inclusive range [lower, upper], specified as values relative to the current
 * offset in the partition. They can be numeric, "current", or "unbounded".
 *
 * The skip/limit values are calculated from the given bounds and the current index.
 *
 * 'defaultValue' is used in cases when the skip/limit combination result in $group not getting any
 * documents. The most likely scenario is that the window has gone off the side of the partition.
 *
 * Note that this function assumes that the data in 'coll' has been seeded with the documents from
 * the seedWithTickerData() method above.
 */
function computeAsGroup(
    {coll, partitionKey, accum, bounds, indexInPartition, defaultValue = null}) {
    const skip = calculateSkip(bounds[0], indexInPartition);
    const limit = calculateLimit(bounds[0], bounds[1], indexInPartition);
    if (skip < 0 || limit <= 0)
        return defaultValue;
    let prefixPipe = [{$match: partitionKey}, {$sort: {_id: 1}}, {$skip: skip}];

    // Only attach a $limit if there's a numeric upper bound (or "current"), since "unbounded"
    // implies an infinite limit.
    if (limit != "unbounded")
        prefixPipe = prefixPipe.concat([{$limit: limit}]);

    const result =
        coll.aggregate(prefixPipe.concat([{$group: {_id: null, res: {[accum]: "$price"}}}]))
            .toArray();
    // If the window is completely off the edge of the right side of the partition, return null.
    if (result.length == 0) {
        return defaultValue;
    }
    return result[0].res;
}

/**
 * Helper to calculate the correct skip based on the lowerBound given.
 */
function calculateSkip(lowerBound, indexInPartition) {
    let skipValueToUse = 0;
    if (lowerBound === "current") {
        skipValueToUse = indexInPartition;
    } else if (lowerBound === "unbounded") {
        skipValueToUse = 0;
    } else {
        skipValueToUse = indexInPartition + lowerBound;
        if (skipValueToUse < 0) {
            skipValueToUse = 0;
        }
    }
    return skipValueToUse;
}

/**
 * Helper to calculate the correct limit based on the bounds given.
 */
function calculateLimit(lowerBound, upperBound, indexInPartition) {
    let limitValueToUse = "unbounded";
    if (upperBound === "current") {
        if (lowerBound === "unbounded") {
            limitValueToUse = indexInPartition + 1;
        } else if (lowerBound === "current") {
            limitValueToUse = 1;
        } else {
            limitValueToUse = Math.abs(lowerBound) + 1;
        }
    } else if (upperBound !== "unbounded") {
        // Keep unbounded as is to not add a limit stage at all later.
        if (lowerBound < 0) {
            // If we don't have a full window in this partition yet.
            if (Math.abs(lowerBound) > indexInPartition) {
                // Either take all documents we've seen if our right bound is also negative, or only
                // do look ahead.
                limitValueToUse =
                    upperBound <= 0 ? indexInPartition : indexInPartition + upperBound + 1;
            } else {
                limitValueToUse = Math.abs(lowerBound) + upperBound + 1;
            }
        } else {
            if (lowerBound === "unbounded") {
                // Only base upper limit on look ahead.
                limitValueToUse = indexInPartition + upperBound + 1;
            } else if (lowerBound === "current") {
                limitValueToUse = upperBound + 1;
            } else {
                // Sliding window uses both bounds for limit.
                limitValueToUse = upperBound - lowerBound + 1;
            }
        }
    }
    return limitValueToUse;
}

function assertResultsEqual(wfRes, index, groupRes, accum) {
    // On DEBUG builds, the computed $group may be slightly different due to precision
    // loss when spilling to disk.
    // TODO SERVER-42616: Enable the exact check for $stdDevPop/Samp.
    if (accum == "$stdDevSamp" || accum == "$stdDevPop") {
        assert.close(groupRes,
                     wfRes.res,
                     "Window function $stdDev result for index " + index + ": " + tojson(wfRes),
                     10 /* 10 decimal places */);
    } else if (accum == "$addToSet") {
        // Order doesn't matter for $addToSet.
        assert(arrayEq(groupRes, wfRes.res),
               "Window function $addToSet results for index " + index + ": " + tojson(wfRes) +
                   "\nexpected:\n " + tojson(groupRes));
    } else
        assert.eq(groupRes,
                  wfRes.res,
                  "Window function result for index " + index + ": " + tojson(wfRes));
}

function assertExplainResult(explainResult) {
    const stages = getAggPlanStages(explainResult, "$_internalSetWindowFields");
    for (let stage of stages) {
        assert(stage.hasOwnProperty("$_internalSetWindowFields"), stage);

        assert(stage.hasOwnProperty("maxFunctionMemoryUsageBytes"), stage);
        const maxFunctionMemUsages = stage["maxFunctionMemoryUsageBytes"];
        for (let field of Object.keys(maxFunctionMemUsages)) {
            assert.gte(maxFunctionMemUsages[field],
                       0,
                       "invalid memory usage for '" + field + "': " + tojson(stage));
        }
        assert.gt(
            stage["maxTotalMemoryUsageBytes"], 0, "Incorrect total mem usage: " + tojson(stage));
        // No test should be using more than 1GB of memory. This is mostly a sanity check that
        // integer underflow doesn't occur.
        assert.lt(stage["maxTotalMemoryUsageBytes"],
                  1 * 1024 * 1024 * 1024,
                  "Incorrect total mem usage: " + tojson(stage));
    }
}

/**
 * Runs the given 'accum' as both a window function and its equivalent accumulator in $group across
 * various combinations of partitioning and window bounds. Asserts that the results are consistent.
 *
 * Note that this function assumes that the documents in 'coll' were initialized using the
 * seedWithTickerData() method above.
 */
function testAccumAgainstGroup(coll, accum, onNoResults = null) {
    forEachPartitionCase(function(partition) {
        documentBounds.forEach(function(bounds, index) {
            jsTestLog("Testing accumulator " + accum + " against " + tojson(partition) +
                      " partition and [" + bounds + "] bounds");

            const pipeline = [
                {
                    $setWindowFields: {
                        partitionBy: partition,
                        sortBy: {_id: 1},
                        output: {res: {[accum]: "$price", window: {documents: bounds}}}
                    },
                },
            ];
            const wfResults = coll.aggregate(pipeline, {allowDiskUse: true}).toArray();
            for (let index = 0; index < wfResults.length; index++) {
                const wfRes = wfResults[index];

                let indexInPartition = (partition === null) ? index : wfRes.partIndex;
                let groupRes;
                if (partition == null) {
                    groupRes = computeAsGroup({
                        coll: coll,
                        partitionKey: {},
                        accum: accum,
                        bounds: bounds,
                        indexInPartition: indexInPartition,
                        defaultValue: onNoResults
                    });
                } else {
                    groupRes = computeAsGroup({
                        coll: coll,
                        partitionKey: {ticker: wfRes.ticker},
                        accum: accum,
                        bounds: bounds,
                        indexInPartition: indexInPartition,
                        defaultValue: onNoResults
                    });
                }

                assertResultsEqual(wfRes, index, groupRes, accum);
            }

            // Run the same pipeline with explain verbosity "executionStats" and verify that the
            // reported metrics are sensible.
            assertExplainResult(
                coll.explain("executionStats").aggregate(pipeline, {allowDiskUse: true}));

            jsTestLog("Done");
        });

        // To get additional coverage, specifically regarding the expiration policy, test
        // combinations of various window types in the same $setWindowFields stage. This is more of
        // a fuzz test so no need to check results.
        forEachDocumentBoundsCombo(function(arrayOfBounds) {
            jsTestLog("Testing accumulator " + accum +
                      " against multiple bounds: " + tojson(arrayOfBounds));
            let baseSpec = {
                partitionBy: partition,
                sortBy: {_id: 1},
            };
            let outputFields = {};
            arrayOfBounds.forEach(function(bounds, index) {
                outputFields["res" + index] = {[accum]: "$price", window: {documents: bounds}};
            });
            let specWithOutput = Object.merge(baseSpec, {output: outputFields});
            const wfResults =
                coll.aggregate([{$setWindowFields: specWithOutput}], {allowDiskUse: true})
                    .toArray();
            assert.gt(wfResults.length, 0);
            jsTestLog("Done");
        });
    });
}
