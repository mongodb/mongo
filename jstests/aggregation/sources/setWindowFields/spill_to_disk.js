/*
 * Test that spilling to disk in $setWindowFields works and returns the correct results.
 * @tags: [
 * requires_fcv_70,
 * requires_profiling,
 * assumes_read_concern_unchanged,
 * do_not_wrap_aggregations_in_facets,
 * featureFlagApproxPercentiles
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findNonConfigNodes.
load("jstests/aggregation/extras/window_function_helpers.js");
load("jstests/libs/analyze_plan.js");         // For getAggPlanStages().
load("jstests/aggregation/extras/utils.js");  // arrayEq.
load("jstests/libs/profiler.js");             // getLatestProfileEntry.

const origParamValue = assert.commandWorked(db.adminCommand({
    getParameter: 1,
    internalDocumentSourceSetWindowFieldsMaxMemoryBytes: 1
}))["internalDocumentSourceSetWindowFieldsMaxMemoryBytes"];
const coll = db[jsTestName()];
coll.drop();

function checkProfilerForDiskWrite(dbToCheck, expectedFirstStage) {
    if (!FixtureHelpers.isMongos(dbToCheck)) {
        const profileObj = getLatestProfilerEntry(dbToCheck, {usedDisk: true});
        jsTestLog(profileObj);
        // Verify that this was a $setWindowFields stage as expected.
        if (profileObj.hasOwnProperty("originatingCommand")) {
            assert(profileObj.originatingCommand.pipeline[0].hasOwnProperty(expectedFirstStage));
        } else if (profileObj.hasOwnProperty("command")) {
            assert(profileObj.command.pipeline[0].hasOwnProperty(expectedFirstStage));
        } else {
            assert(false, "Profiler should have had command field", profileObj);
        }
    }
}

function resetProfiler(db) {
    FixtureHelpers.runCommandOnEachPrimary({db: db, cmdObj: {profile: 0}});
    db.system.profile.drop();
    FixtureHelpers.runCommandOnEachPrimary({db: db, cmdObj: {profile: 2}});
}

// Doc size was found through logging the size in the SpillableCache. Partition sizes were chosen
// arbitrarily.
let avgDocSize = 171;
let smallPartitionSize = 6;
let largePartitionSize = 21;
// The number 600 was chosen by observing how much memory is required for the accumulators to run
// on all windows (~1600 bytes).
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       avgDocSize * smallPartitionSize + 600);

seedWithTickerData(coll, 10);

// Run $sum test with memory limits that cause spilling to disk.
testAccumAgainstGroup(coll, "$sum", 0);

// Run a $percentile test that fails since we go over the memory limit allowed and can't spill.
let errorPipeline = [
    {
        $setWindowFields: {
            partitionBy: "$partition",
            sortBy: {partition: 1},
            output: {
                p: {
                    $percentile: {p: [0.9], input: "$price", method: "approximate"},
                    window: {documents: [0, "unbounded"]}
                }
            }
        }
    },
    {$sort: {_id: 1}}
];
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), pipeline: errorPipeline, allowDiskUse: false, cursor: {}}),
    5643011);

// Run $percentile test with memory limits that cause spilling to disk and assert it succeeds.
// In the test suite below, we will run a query identical to the one that failed above.
resetProfiler(db);
testAccumAgainstGroup(
    coll, "$percentile", [null], {p: [0.9], input: "$price", method: "approximate"});
// Confirm that spilling did occur.
checkProfilerForDiskWrite(db, "$setWindowFields");

// Run $median test with memory limits that cause spilling to disk.
resetProfiler(db);
testAccumAgainstGroup(coll, "$median", null, {input: "$price", method: "approximate"});
// Confirm that spilling did occur.
checkProfilerForDiskWrite(db, "$setWindowFields");

// Test that a query that spills to disk succeeds across getMore requests.
// The next test uses less memory. Reduce memory limit to ensure spilling occurs. The number 70 was
// chosen by observing how much memory is required for the test to run.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       avgDocSize * smallPartitionSize + 70);
resetProfiler(db);
const wfResults =
    coll.aggregate(
            [
                {
                    $setWindowFields: {
                        sortBy: {_id: 1},
                        output: {res: {$sum: "$price", window: {documents: ["unbounded", 5]}}}
                    },
                },
            ],
            {allowDiskUse: true, cursor: {batchSize: 1}})
        .toArray();
assert.eq(wfResults.length, 20);
checkProfilerForDiskWrite(db, "$setWindowFields");

// Test a small, in memory, partition and a larger partition that requires spilling to disk.
coll.drop();
// Create small partition.
for (let i = 0; i < smallPartitionSize; i++) {
    assert.commandWorked(coll.insert({_id: i, val: i, partition: 1}));
}
// Create large partition.
for (let i = 0; i < largePartitionSize; i++) {
    assert.commandWorked(coll.insert({_id: i + smallPartitionSize, val: i, partition: 2}));
}

// Run an aggregation that will keep all documents in the cache for all documents.
resetProfiler(db);
let results =
    coll.aggregate(
            [
                {
                    $setWindowFields: {
                        partitionBy: "$partition",
                        sortBy: {partition: 1},
                        output: {
                            sum: {
                                $sum: "$val",
                                window: {documents: [-largePartitionSize, largePartitionSize]}
                            }
                        }
                    }
                },
                {$sort: {_id: 1}}
            ],
            {allowDiskUse: true})
        .toArray();
for (let i = 0; i < results.length; i++) {
    if (results[i].partition === 1) {
        assert.eq(results[i].sum, 15, "Unexepected result in first partition at position " + i);
    } else {
        assert.eq(results[i].sum, 210, "Unexepcted result in second partition at position " + i);
    }
}
checkProfilerForDiskWrite(db, "$setWindowFields");

// We don't execute setWindowFields in a sharded explain.
if (!FixtureHelpers.isMongos(db)) {
    // Test that an explain that executes the query reports usedDisk correctly.
    let explainPipeline = [
        {
            $setWindowFields: {
                partitionBy: "$partition",
                sortBy: {partition: 1},
                output: {arr: {$sum: "$val", window: {documents: [-21, 21]}}}
            }
        },
        {$sort: {_id: 1}}
    ];

    let stages = getAggPlanStages(
        coll.explain("allPlansExecution").aggregate(explainPipeline, {allowDiskUse: true}),
        "$_internalSetWindowFields");
    assert(stages[0]["usedDisk"], stages);
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                           "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                           avgDocSize * largePartitionSize * 2);
    explainPipeline = [
        {
            $setWindowFields: {
                partitionBy: "$partition",
                sortBy: {partition: 1},
                output: {arr: {$sum: "$val", window: {documents: [0, 0]}}}
            }
        },
        {$sort: {_id: 1}}
    ];

    stages = getAggPlanStages(
        coll.explain("allPlansExecution").aggregate(explainPipeline, {allowDiskUse: true}),
        "$_internalSetWindowFields");
    assert(!stages[0]["usedDisk"], stages);
}

// Run an aggregation that will store too many documents in the function and force a spill. Set the
// memory limit to be over the size of the large partition.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       largePartitionSize * avgDocSize + 1);
resetProfiler(db);
results = coll.aggregate(
                  [
                      {
                          $setWindowFields: {
                              partitionBy: "$partition",
                              sortBy: {partition: 1},
                              output: {arr: {$push: "$val", window: {documents: [-25, 25]}}}
                          }
                      },
                      {$sort: {_id: 1}}
                  ],
                  {allowDiskUse: true})
              .toArray();
checkProfilerForDiskWrite(db, "$setWindowFields");
for (let i = 0; i < results.length; i++) {
    if (results[i].partition === 1) {
        assert(arrayEq(results[i].arr, [0, 1, 2, 3, 4, 5]),
               "Unexepected result in first partition at position " + i);
    } else {
        assert(arrayEq(results[i].arr,
                       [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]),
               "Unexepcted result in second partition at position " + i);
    }
}

// Check that if function memory limit exceeds we fail even though the partition iterator spilled.
// $push uses about ~950 to store all the values in the second partition.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       avgDocSize * 2);

function runExceedMemoryLimitTest(spec) {
    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$setWindowFields: {partitionBy: "$partition", sortBy: {partition: 1}, output: spec}},
            {$sort: {_id: 1}}
        ],
        allowDiskUse: true,
        cursor: {}
    }),
                                 5414201);
}

runExceedMemoryLimitTest({arr: {$push: "$val", window: {documents: [-21, 21]}}});
runExceedMemoryLimitTest({
    percentile: {
        $percentile: {p: [0.6, 0.7], input: "$price", method: "approximate"},
        window: {documents: [-21, 21]}
    }
});

coll.drop();
// Test that situations that would require a large spill successfully write to disk.
// Set the limit to spill after ~1000 documents since that is the batch size when we write to disk.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       1000 * avgDocSize);
let numDocs = 1111;
let batchArr = [];
for (let docNum = 0; docNum < numDocs; docNum++) {
    batchArr.push({_id: docNum, val: docNum, partition: 1});
}
assert.commandWorked(coll.insert(batchArr));
// Run a document window over the whole collection to keep everything in the cache.
resetProfiler(db);
results =
    coll.aggregate(
            [
                {
                    $setWindowFields: {
                        // partitionBy: "$partition",
                        sortBy: {partition: 1},
                        output: {arr: {$sum: "$val", window: {documents: [-numDocs, numDocs]}}}
                    }
                },
                {$sort: {_id: 1}}
            ],
            {allowDiskUse: true})
        .toArray();
checkProfilerForDiskWrite(db, "$setWindowFields");
// Check that the command succeeded.
assert.eq(results.length, numDocs);
for (let i = 0; i < numDocs; i++) {
    assert.eq(results[i].arr, 616605, results);
}

// Test that usedDisk true is set when spilling occurs inside $lookup subpipline.
// Lower the memory limit to ensure spilling occurs.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       500);
resetProfiler(db);
coll.aggregate(
        [
            {
                $lookup: {
                    from: coll.getName(),
                    as: "same",
                    pipeline: [{
                        $setWindowFields: {
                            sortBy: { _id: 1 },
                            output: { res: { $sum: "$price", window: { documents: ["unbounded", 5] } } }
                        },
                    }],
                }
            }],
        { allowDiskUse: true, cursor: {} })
        .toArray();
checkProfilerForDiskWrite(db, "$lookup");

// Reset limit for other tests.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       origParamValue);
// Reset profiler.
FixtureHelpers.runCommandOnEachPrimary({db: db, cmdObj: {profile: 0}});
})();
