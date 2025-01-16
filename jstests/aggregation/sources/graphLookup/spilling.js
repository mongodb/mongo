// This test checks $graphLookup spilling capabilities
// @tags: [
//   requires_fcv_81,
//   requires_getmore,
//   requires_persistence,
//   requires_pipeline_optimization,
//   not_allowed_with_signed_security_token,
//   uses_getmore_outside_of_transaction,
// ]

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

var local = db.local;
var foreign = db.foreign;

function drop() {
    local.drop();
    foreign.drop();
}

const bigStr = Array(1024 * 1024).toString();  // ~ 1MB of ','

function getKnob(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setKnob(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

function runPipelineAndCheckUsedDiskValue(pipeline, expectSpilling = true) {
    const explain = local.explain("executionStats").aggregate(pipeline);
    const graphLookupExplains = getAggPlanStages(explain, "$graphLookup");
    // If there are no $graphLookup explains, it means it was done in the merge part of the pipeline
    // and we don't have execution stats for it
    if (graphLookupExplains.length !== 0) {
        const filteredExplains = graphLookupExplains.filter((e) => e.nReturned > 0);
        assert.eq(filteredExplains.length,
                  1,
                  "Expected only one shard to return data. Found: " + tojson(graphLookupExplains));
        assert.eq(
            filteredExplains[0].usedDisk,
            expectSpilling,
            "Expected usedDisk: " + expectSpilling + ". Found: " + tojson(graphLookupExplains));
    }

    return local.aggregate(pipeline).toArray();
}

function testKnobsAndVisitedSpilling() {
    drop();

    const docCount = 120;
    const docs = [];
    for (let i = 0; i < docCount; ++i) {
        docs.push({_id: i, to: i + 1, payload: bigStr});
    }
    assert.commandWorked(foreign.insertMany(docs));
    assert.commandWorked(local.insertOne({start: 0}));

    let pipeline = [
        {
            $graphLookup: {
                from: "foreign",
                startWith: "$start",
                connectFromField: "to",
                connectToField: "_id",
                as: "output"
            }
        }
    ];

    // Without $unwind even with spilling we will fail to generate output.
    assert.throwsWithCode(() => local.aggregate(pipeline).toArray(), 8442700);

    function testGraphLookupWorksWithoutAbsorbingUnwind(pipeline, options) {
        // $project should prevent $graphLookup from absorbing $unwind.
        pipeline = pipeline.concat([
            {
                $project: {
                    output: {
                        $sortArray: {
                            input: "$output",
                            sortBy: {_id: 1},
                        }
                    }
                }
            },
            {$unwind: "$output"},
            {$replaceRoot: {newRoot: "$output"}},
        ]);

        let expectSpilling = !(options.allowDiskUse === false);
        assert.eq(runPipelineAndCheckUsedDiskValue(pipeline, expectSpilling), docs);
    }

    const resultSizeLimitKnob = "internalGraphLookupStageIntermediateDocumentMaxSizeBytes";
    const originalResultSizeLimit = getKnob(resultSizeLimitKnob);
    try {
        // Raise result size limit to fit everything without absorbing $unwind.
        setKnob(resultSizeLimitKnob, 500 * 1024 * 1024);
        testGraphLookupWorksWithoutAbsorbingUnwind(pipeline, {} /*options*/);

        // Even with raised result size limit, we still hit memory limit and should fail without
        // spilling.
        const disableSpillingOptions = {allowDiskUse: false};
        assert.throwsWithCode(() => local.aggregate(pipeline, disableSpillingOptions).toArray(),
                              ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

        const memoryLimitKnob = "internalDocumentSourceGraphLookupMaxMemoryBytes";
        const originalMemoryLimitValue = getKnob(memoryLimitKnob);
        try {
            // Raise memory limit to fit everything without spilling.
            setKnob(memoryLimitKnob, 500 * 1024 * 1024);
            testGraphLookupWorksWithoutAbsorbingUnwind(pipeline, disableSpillingOptions);
        } finally {
            setKnob(memoryLimitKnob, originalMemoryLimitValue);
        }
    } finally {
        setKnob(resultSizeLimitKnob, originalResultSizeLimit);
    }

    pipeline.push({$unwind: "$output"});
    pipeline.push({$replaceRoot: {newRoot: "$output"}});
    pipeline.push({$sort: {_id: 1}});

    // With default knob values, pipeline with $unwind should work and spill.
    assert.eq(runPipelineAndCheckUsedDiskValue(pipeline), docs);
}
testKnobsAndVisitedSpilling();

function testQueueSpilling() {
    drop();

    const docCount = 128;
    const docs = [];
    for (let i = 1; i <= docCount; ++i) {
        // Adding payload to _id so queue also have to be spilled.
        docs.push({
            _id: {index: i, payload: bigStr},
            to: [{index: 2 * i, payload: bigStr}, {index: 2 * i + 1, payload: bigStr}]
        });
    }
    foreign.insertMany(docs);
    local.insertOne({start: {index: 1, payload: bigStr}});

    const pipeline = [
        {
            $graphLookup: {
                from: "foreign",
                startWith: "$start",
                connectFromField: "to",
                connectToField: "_id",
                depthField: "depth",
                as: "output"
            }
        },
        {$unwind: "$output"},
        {$replaceRoot: {newRoot: "$output"}},
        {$sort: {_id: 1}}
    ];

    const memoryLimitKnob = "internalDocumentSourceGraphLookupMaxMemoryBytes";
    const originalMemoryLimitValue = getKnob(memoryLimitKnob);
    try {
        // Lower the limit to guarantee queue spilling
        setKnob(memoryLimitKnob, 500 * 1024 * 1024);

        const results = runPipelineAndCheckUsedDiskValue(pipeline);
        assert.eq(results.length, docCount);
        for (let result of results) {
            assert.eq(result.depth, Math.trunc(Math.log2(result._id.index)), result);
        }
    } finally {
        setKnob(memoryLimitKnob, originalMemoryLimitValue);
    }
}
testQueueSpilling();
