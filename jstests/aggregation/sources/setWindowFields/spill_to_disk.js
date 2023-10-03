/*
 * Test that spilling to disk in $setWindowFields works and returns the correct results.
 * @tags: [
 * requires_fcv_70,
 * requires_profiling,
 * assumes_read_concern_unchanged,
 * do_not_wrap_aggregations_in_facets,
 * ]
 */
import "jstests/libs/sbe_assert_error_override.js";

import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {
    seedWithTickerData,
    testAccumAgainstGroup
} from "jstests/aggregation/extras/window_function_helpers.js";
import {getAggPlanStages} from "jstests/libs/analyze_plan.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// Doc size was found through logging the size in the SpillableCache. Partition sizes were chosen
// arbitrarily.
const avgDocSize = 171;
const smallPartitionSize = 6;
const largePartitionSize = 21;
const coll = db[jsTestName()];
const admin = db.getSiblingDB("admin");

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

function changeSpillLimit({mode, maxDocs}) {
    FixtureHelpers.runCommandOnEachPrimary({
        db: admin,
        cmdObj: {
            configureFailPoint: 'overrideMemoryLimitForSpill',
            mode: mode,
            'data': {maxDocsBeforeSpill: maxDocs}
        }
    });
    FixtureHelpers.runCommandOnEachPrimary({
        db: admin,
        cmdObj: {
            configureFailPoint: 'overrideMemoryLimitForSpillForSBEWindowStage',
            mode: mode,
            'data': {spillCounter: maxDocs}
        }
    });
}

function testSingleAccumulator(accumulator, nullValue, spec) {
    resetProfiler(db);
    testAccumAgainstGroup(coll, accumulator, nullValue, spec);
    checkProfilerForDiskWrite(db, "$setWindowFields");
}

// Assert that spilling to disk doesn't affect the correctness of different accumulators.
function testSpillWithDifferentAccumulators() {
    coll.drop();
    seedWithTickerData(coll, 10);

    // Spill to disk after 5 documents.
    changeSpillLimit({mode: 'alwaysOn', maxDocs: 5});

    testSingleAccumulator("$sum", 0, "$price");
    testSingleAccumulator(
        "$percentile", [null], {p: [0.9], input: "$price", method: "approximate"});
    testSingleAccumulator("$median", null, {input: "$price", method: "approximate"});

    // Assert that spilling works across 'getMore' commands
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

    // Turn off the failpoint for future tests.
    changeSpillLimit({mode: 'off', maxDocs: null});
}

// Assert a small, in memory, partition and a larger partition that requires spilling to disk
// returns correct results.
function testSpillWithDifferentPartitions() {
    // Spill to disk after 5 documents. This number should be less than 'smallPartitionSize'.
    changeSpillLimit({mode: 'alwaysOn', maxDocs: 5});

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
            assert.eq(results[i].sum, 15, "Unexpected result in first partition at position " + i);
        } else {
            assert.eq(
                results[i].sum, 210, "Unexpected result in second partition at position " + i);
        }
    }
    checkProfilerForDiskWrite(db, "$setWindowFields");

    // Run an aggregation that will store too many documents in the function and force a spill.
    // Spill to disk after 10 documents.
    changeSpillLimit({mode: 'alwaysOn', maxDocs: 10});
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
                   "Unexpected result in first partition at position " + i);
        } else {
            assert(
                arrayEq(results[i].arr,
                        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]),
                "Unexpected result in second partition at position " + i);
        }
    }

    // Turn off the failpoint for future tests.
    changeSpillLimit({mode: 'off', maxDocs: null});
}

// Assert that 'usedDisk' is correctly set in an explain query.
function testUsedDiskAppearsInExplain() {
    // Don't drop the collection, since the set up in spillWithDifferentPartitions() is valid.

    // Spill after 10 documents. This number should be bigger than the window size.
    changeSpillLimit({mode: 'alwaysOn', maxDocs: 10});

    // Run an explain query where 'usedDisk' should be true.
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

    // Run an explain query with the default memory limit, so 'usedDisk' should be false.
    changeSpillLimit({mode: 'off', maxDocs: null});
    stages = getAggPlanStages(
        coll.explain("allPlansExecution").aggregate(explainPipeline, {allowDiskUse: true}),
        "$_internalSetWindowFields");
    assert(!stages[0]["usedDisk"], stages);
}

// Assert that situations that would require a large spill successfully write to disk.
function testLargeSpill() {
    coll.drop();

    let numDocs = 1111;
    let batchArr = [];
    for (let docNum = 0; docNum < numDocs; docNum++) {
        batchArr.push({_id: docNum, val: docNum, partition: 1});
    }
    assert.commandWorked(coll.insert(batchArr));
    // Spill to disk after 1000 documents.
    changeSpillLimit({mode: 'alwaysOn', maxDocs: 1000});

    // Run a document window over the whole collection to keep everything in the cache.
    resetProfiler(db);
    const results =
        coll.aggregate(
                [
                    {
                        $setWindowFields: {
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

    // Turn off the failpoint for future tests.
    changeSpillLimit({mode: 'off', maxDocs: null});
}

// Assert that usedDisk true is set to true if spilling occurs inside $lookup subpipline.
function testUsedDiskInLookupPipeline() {
    coll.drop();
    for (let i = 0; i < largePartitionSize; i++) {
        assert.commandWorked(coll.insert({_id: i, val: i}));
    }
    // Spill to disk after 5 documents.
    changeSpillLimit({mode: 'alwaysOn', maxDocs: 5});

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

    // Turn off the failpoint for future tests.
    changeSpillLimit({mode: 'off', maxDocs: null});
}

function runSingleErrorTest({spec, errorCode, diskUse}) {
    assert.commandFailedWithCode(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {$setWindowFields: {partitionBy: "$partition", sortBy: {partition: 1}, output: spec}},
            {$sort: {_id: 1}}
        ],
        allowDiskUse: diskUse,
        cursor: {}
    }),
                                 errorCode);
}

// Assert that an error is raised when the pipeline exceeds the memory limit or disk use is not
// allowed.
function testErrorsWhenCantSpill() {
    // Don't drop the collection, since the set up in testUsedDiskInLookupPipeline() is valid.

    const origParamValue = assert.commandWorked(db.adminCommand({
        getParameter: 1,
        internalDocumentSourceSetWindowFieldsMaxMemoryBytes: 1
    }))["internalDocumentSourceSetWindowFieldsMaxMemoryBytes"];
    // Decrease the maximum memory limit allowed. $push uses about ~950 to store all the values in
    // the second partition.
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                           "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                           avgDocSize * 2);

    // Assert the pipeline errors when exceeding maximum memory, even though the data spilled.
    runSingleErrorTest({
        spec: {arr: {$push: "$val", window: {documents: [-21, 21]}}},
        errorCode: 5414201,
        diskUse: true
    });
    // Assert the pipeline errors when exceeding the maximum memory, even though the data spilled.
    let percentileSpec = {
        $percentile: {p: [0.6, 0.7], input: "$price", method: "approximate"},
        window: {documents: [-21, 21]}
    };
    runSingleErrorTest({spec: {percentile: percentileSpec}, errorCode: 5414201, diskUse: true});
    // Assert the pipeline fails when trying to spill, but 'allowDiskUse' is set to false.
    runSingleErrorTest({spec: {percentile: percentileSpec}, errorCode: 5643011, diskUse: false});
    // Reset the memory limit for other tests.
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                           "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                           origParamValue);
}

// Run the tests.
testSpillWithDifferentAccumulators();
testSpillWithDifferentPartitions();
// We don't execute setWindowFields in a sharded explain.
// TODO SERVER-78714: Implement explain for $setWindowFields.
if (!FixtureHelpers.isMongos(db) && !checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    testUsedDiskAppearsInExplain();
}
testLargeSpill();
testUsedDiskInLookupPipeline();
testErrorsWhenCantSpill();

// Reset profiler.
FixtureHelpers.runCommandOnEachPrimary({db: db, cmdObj: {profile: 0}});
