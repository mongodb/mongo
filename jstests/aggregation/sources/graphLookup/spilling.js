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

function getKnob(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setKnob(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

const string1MB = Array(1024 * 1024).toString();
const string1KB = Array(1024).toString();

function testResultSizeLimitKnob() {
    drop();

    const resultSizeLimitKnob = "internalGraphLookupStageIntermediateDocumentMaxSizeBytes";
    const originalResultSizeLimit = getKnob(resultSizeLimitKnob);
    setKnob(resultSizeLimitKnob, 17 * 1024 * 1024);

    const docCount = 18;
    for (let i = 0; i < docCount; ++i) {
        assert.commandWorked(foreign.insertOne({_id: i, to: i + 1, payload: string1MB}));
    }
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

    assert.throwsWithCode(() => local.aggregate(pipeline).itcount(), 8442700);

    pipeline.push({$unwind: "$output"});
    pipeline.push({$project: {payload: 0, "output.payload": 0}});

    assert.eq(local.aggregate(pipeline).itcount(), docCount);

    setKnob(resultSizeLimitKnob, originalResultSizeLimit);
}
testResultSizeLimitKnob();

function assertFieldPositive(fieldName, filteredExplain, fullExplain) {
    assert.gt(
        filteredExplain[fieldName], 0, `Expected ${fieldName} > 0. Found: ` + tojson(fullExplain));
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
        if (expectSpilling) {
            assertFieldPositive("spills", filteredExplains[0], explain);
            assertFieldPositive("spilledBytes", filteredExplains[0], explain);
            assertFieldPositive("spilledRecords", filteredExplains[0], explain);
            assertFieldPositive("spilledDataStorageSize", filteredExplains[0], explain);
        }
    }

    return local.aggregate(pipeline);
}

const memoryLimitKnob = "internalDocumentSourceGraphLookupMaxMemoryBytes";
const originalMemoryLimitKnobValues = getKnob(memoryLimitKnob);
setKnob(memoryLimitKnob, 50 * 1024);

function testKnobsAndVisitedSpilling() {
    drop();

    const docCount = 100;
    const docs = [];
    for (let i = 0; i < docCount; ++i) {
        docs.push({_id: i, to: i + 1, payload: string1KB});
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

    function assertCorrectResult(cursor) {
        let index = 0;
        while (cursor.hasNext()) {
            const doc = cursor.next();
            assert.eq(doc, docs[index]);
            index++;
        }
        assert.eq(index, docCount);
    }

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
        assertCorrectResult(runPipelineAndCheckUsedDiskValue(pipeline, expectSpilling));
    }

    testGraphLookupWorksWithoutAbsorbingUnwind(pipeline, {} /*options*/);

    // We hit memory limit and should fail without spilling.
    const disableSpillingOptions = {allowDiskUse: false};
    assert.throwsWithCode(() => local.aggregate(pipeline, disableSpillingOptions).itcount(),
                          ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    const previousMemoryLimitKnobValue = getKnob(memoryLimitKnob);
    setKnob(memoryLimitKnob, 100 * 1024 * 1024);
    testGraphLookupWorksWithoutAbsorbingUnwind(pipeline, disableSpillingOptions);

    setKnob(memoryLimitKnob, previousMemoryLimitKnobValue);

    pipeline.push({$unwind: "$output"});
    pipeline.push({$replaceRoot: {newRoot: "$output"}});
    pipeline.push({$sort: {_id: 1}});

    // With restored knob values, pipeline with $unwind should work and spill.
    assertCorrectResult(runPipelineAndCheckUsedDiskValue(pipeline));
}
testKnobsAndVisitedSpilling();

function testQueueSpilling() {
    drop();

    const docCount = 128;
    for (let i = 1; i <= docCount; ++i) {
        // Adding payload to _id so queue also have to be spilled.
        assert.commandWorked(db.foreign.insertOne({
            _id: {index: i, payload: string1KB},
            to: [{index: 2 * i, payload: string1KB}, {index: 2 * i + 1, payload: string1KB}]
        }));
    }
    local.insertOne({start: {index: 1, payload: string1KB}});

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
        { $unwind: "$output" },
        { $replaceRoot: { newRoot: "$output" } },
        {$sort: {_id: 1}},
    ];

    const c = runPipelineAndCheckUsedDiskValue(pipeline);
    let count = 0;
    while (c.hasNext()) {
        const doc = c.next();
        count++;
        assert.eq(doc._id.index, count, doc);
        assert.eq(doc.depth, Math.trunc(Math.log2(doc._id.index)), doc);
    }
    assert.eq(count, docCount);
}
testQueueSpilling();

setKnob(memoryLimitKnob, originalMemoryLimitKnobValues);
