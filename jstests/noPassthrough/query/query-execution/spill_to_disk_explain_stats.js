import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {setParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const collName = jsTestName();
const coll = db[collName];
coll.drop();
const foreignCollName = jsTestName() + "_foreign";
const foreignColl = db[foreignCollName];
foreignColl.drop();
const isSbeEnabled = checkSbeFullyEnabled(db);

// Set up relevant query knobs so that the query will spill for every document.
// No batching in document source cursor
assert.commandWorked(setParameter(db, "internalDocumentSourceCursorInitialBatchSize", 1));
assert.commandWorked(setParameter(db, "internalDocumentSourceCursorBatchSizeBytes", 1));
// Spilling memory threshold for $sort
assert.commandWorked(setParameter(db, "internalQueryMaxBlockingSortMemoryUsageBytes", 1));
// Spilling memory threshold for $group
assert.commandWorked(setParameter(db, "internalDocumentSourceGroupMaxMemoryBytes", 1));
assert.commandWorked(
    setParameter(db, "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 1),
);
// Spilling memory threshold for $setWindowFields
assert.commandWorked(setParameter(db, "internalDocumentSourceSetWindowFieldsMaxMemoryBytes", 472));
// Spilling memory threshold for $bucketAuto
assert.commandWorked(setParameter(db, "internalDocumentSourceBucketAutoMaxMemoryBytes", 1));
// Spilling memory threshold for $lookup and $lookup-$unwind
assert.commandWorked(
    setParameter(
        db,
        "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill",
        1,
    ),
);
// Spilling memory threshold for $graphLookup
assert.commandWorked(setParameter(db, "internalDocumentSourceGraphLookupMaxMemoryBytes", 1));

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(foreignColl.insert({_id: i}));
}

const pipelines = {
    sort: [{$sort: {a: 1}}],
    "group": [{$group: {_id: "$_id", a: {$sum: "$_id"}}}],
    setWindowFields: [
        {
            $setWindowFields: {
                partitionBy: "$_id",
                sortBy: {_id: 1},
                output: {b: {$sum: "$_id", window: {documents: [0, 0]}}},
            },
        },
    ],
    lookup: [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"}}],
    "lookup-unwind": [
        {
            $lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"},
        },
        {$unwind: "$c"},
    ],
    bucketAuto: [{$bucketAuto: {groupBy: "$_id", buckets: 2, output: {count: {$sum: 1}}}}],
    graphLookup: [
        {
            $graphLookup: {
                from: foreignCollName,
                startWith: "$_id",
                connectToField: "_id",
                connectFromField: "_id",
                as: "c",
            },
        },
    ],
};

function assertSpillingStats(explain, classicStageName, SBEStageName) {
    const stageName = explain.explainVersion === "1" ? classicStageName : SBEStageName;

    const stageExplain = getAggPlanStage(explain, stageName);
    assert(
        stageExplain,
        `Did not find stage \"${stageName}\" in explain output ${tojson(explain)}`,
    );
    assert(
        stageExplain.hasOwnProperty("usedDisk"),
        `Did not find stage "usedDisk" stat in explain output ${tojson(stageExplain)}`,
    );
    assert(
        stageExplain.hasOwnProperty("spills"),
        `Did not find stage "spills" stat in explain output ${tojson(stageExplain)}`,
    );
    assert(
        stageExplain.hasOwnProperty("spilledBytes"),
        `Did not find stage "spilledBytes" stat in explain output ${tojson(stageExplain)}`,
    );
    assert(
        stageExplain.hasOwnProperty("spilledRecords"),
        `Did not find stage "spilledRecords" stat in explain output ${tojson(stageExplain)}`,
    );
    assert(
        stageExplain.hasOwnProperty("spilledDataStorageSize"),
        `Did not find stage "spilledDataStorageSize" stat in explain output ${tojson(stageExplain)}`,
    );
}

const spillingFields = ["spills", "spilledBytes", "spilledRecords", "spilledDataStorageSize"];

function totalFieldName(field) {
    return "total" + field.charAt(0).toUpperCase() + field.slice(1);
}

function sumPerStageSpillingStats(
    node,
    sums = Object.fromEntries(spillingFields.map((f) => [f, 0])),
) {
    if (Array.isArray(node)) {
        node.forEach((child) => sumPerStageSpillingStats(child, sums));
    } else if (node !== null && typeof node === "object") {
        // The counters can be plain numbers or NumberLongs, so do not check the type.
        if (node.hasOwnProperty("spills") && !isNaN(Number(node.spills))) {
            for (const field of spillingFields) {
                sums[field] += Number(node[field]);
            }
        }
        for (const key of Object.keys(node)) {
            sumPerStageSpillingStats(node[key], sums);
        }
    }
    return sums;
}

function assertTotalsMatchStages(totalsHolder, perStageSums, explain) {
    const spilled = perStageSums.spills > 0;
    for (const field of spillingFields) {
        const name = totalFieldName(field);
        if (spilled) {
            assert.eq(
                Number(totalsHolder[name]),
                perStageSums[field],
                `Unexpected "${name}" in explain output`,
                {explain},
            );
        } else {
            assert(!totalsHolder.hasOwnProperty(name), `Found "${name}" although nothing spilled`, {
                explain,
            });
        }
    }
}

function assertPipelineTotalSpillingStats(explain) {
    if (explain.stages === undefined) {
        // The whole pipeline was pushed down to the query executor, totals are in the top-level
        // 'executionStats'.
        assertTotalsMatchStages(
            explain.executionStats,
            sumPerStageSpillingStats(explain.executionStats.executionStages),
            explain,
        );
        return explain;
    }

    // The pipeline-wide totals are at the top level of the explain and cover every stage.
    assertTotalsMatchStages(explain, sumPerStageSpillingStats(explain.stages), explain);

    // The '$cursor' stage reports totals for only the part of the pipeline pushed down to the query
    // executor. Per-stage stats under the cursor are in its 'executionStats'.
    const cursorStage = explain.stages[0].$cursor;
    if (cursorStage !== undefined) {
        assertTotalsMatchStages(
            cursorStage.executionStats,
            sumPerStageSpillingStats(cursorStage.executionStats.executionStages),
            explain,
        );
    }
    return explain;
}

function testSpillingStats(pipeline, classicStageName, SBEStageName) {
    jsTest.log.info(`Running pipeline`, pipeline);

    const explain = db[collName]
        .explain("executionStats")
        .aggregate(pipeline, {"allowDiskUse": true});
    assertSpillingStats(explain, classicStageName, SBEStageName);

    assertPipelineTotalSpillingStats(explain);
    // The totals sit at the top level for a 'stages' explain, and in 'executionStats' if the whole
    // pipeline was pushed down to the query executor.
    const totalsHolder = explain.stages === undefined ? explain.executionStats : explain;
    assert(
        totalsHolder.hasOwnProperty("totalSpills"),
        "Expected pipeline-wide spilling totals, since the stage spilled",
        {explain},
    );

    const allPlansExecution = db[collName]
        .explain("allPlansExecution")
        .aggregate(pipeline, {"allowDiskUse": true});
    assertSpillingStats(allPlansExecution, classicStageName, SBEStageName);
}

testSpillingStats(pipelines["sort"], "SORT", "sort"); // classic, sbe
testSpillingStats(pipelines["group"], "$group", "group"); // sbe, classic
// $setWindowFields always runs in classic, even when the preceding $sort is pushed down to SBE.
testSpillingStats(
    pipelines["setWindowFields"],
    "$_internalSetWindowFields",
    "$_internalSetWindowFields",
);
if (isSbeEnabled) {
    testSpillingStats(pipelines["lookup"], "$lookup", "hash_lookup");
    testSpillingStats(pipelines["lookup-unwind"], "$lookup", "hash_lookup_unwind");
}
testSpillingStats(pipelines["bucketAuto"], "$bucketAuto", "$bucketAuto");
testSpillingStats(pipelines["graphLookup"], "$graphLookup", "$graphLookup");

// The totals must equal the per-stage sums when a skip and a limit are applied.
{
    const findExplain = coll.find().sort({a: 1}).skip(2).limit(5).explain("executionStats");
    assertTotalsMatchStages(
        findExplain.executionStats,
        sumPerStageSpillingStats(findExplain.executionStats.executionStages),
        findExplain,
    );
    assertPipelineTotalSpillingStats(
        db[collName]
            .explain("executionStats")
            .aggregate([{$sort: {a: 1}}, {$skip: 2}, {$limit: 5}], {"allowDiskUse": true}),
    );
}

// A spill in a pushed-down '$sort' plus a spill in a later '$bucketAuto' must be counted once each.
{
    const explain = db[collName]
        .explain("executionStats")
        .aggregate([{$sort: {a: 1}}, ...pipelines["bucketAuto"]], {"allowDiskUse": true});
    assertPipelineTotalSpillingStats(explain);
    const cursorTotals = explain.stages[0].$cursor.executionStats;
    assert(cursorTotals.hasOwnProperty("totalSpills"), "Expected the '$cursor' stage to spill", {
        explain,
    });
    assert.gt(
        Number(explain.totalSpills),
        Number(cursorTotals.totalSpills),
        "Pipeline-wide total should also include the stage after '$cursor'",
        {explain},
    );
}

MongoRunner.stopMongod(conn);
