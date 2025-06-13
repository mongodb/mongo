import {
    getAggPlanStage,
} from "jstests/libs/query/analyze_plan.js";
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
    setParameter(db, "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 1));
// Spilling memory threshold for $setWindowFields
assert.commandWorked(setParameter(
    db, "internalDocumentSourceSetWindowFieldsMaxMemoryBytes", isSbeEnabled ? 129 : 392));
// Spilling memory threshold for $bucketAuto
assert.commandWorked(setParameter(db, "internalDocumentSourceBucketAutoMaxMemoryBytes", 1));
// Spilling memory threshold for $lookup and $lookup-$unwind
assert.commandWorked(setParameter(
    db, "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill", 1));
// Spilling memory threshold for $graphLookup
assert.commandWorked(setParameter(db, "internalDocumentSourceGraphLookupMaxMemoryBytes", 1));

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({_id: i}));
    assert.commandWorked(foreignColl.insert({_id: i}));
}

const pipelines = {
    sort: [{$sort: {a: 1}}],
    'group': [{$group: {_id: "$_id", a: {$sum: "$_id"}}}],
    setWindowFields: [{
        $setWindowFields: {
            partitionBy: "$_id",
            sortBy: {_id: 1},
            output: {b: {$sum: "$_id", window: {documents: [0, 0]}}}
        }
    }],
    lookup: [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"}}],
    'lookup-unwind': [{
        $lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "c"}
    },
    {$unwind: "$c"}],
    bucketAuto: [{$bucketAuto: {groupBy: "$_id", buckets: 2, output: {count: {$sum: 1}}}}],
    graphLookup: [{
        $graphLookup: {
            from: foreignCollName,
            startWith: "$_id",
            connectToField: "_id",
            connectFromField: "_id",
            as: "c",
        }
    }],
};

function assertSpillingStats(explain, classicStageName, SBEStageName) {
    const stageName = explain.explainVersion === "1" ? classicStageName : SBEStageName;

    const stageExplain = getAggPlanStage(explain, stageName);
    assert(stageExplain,
           `Did not find stage \"${stageName}\" in explain output ${tojson(explain)}`);
    assert(stageExplain.hasOwnProperty("usedDisk"),
           `Did not find stage "usedDisk" stat in explain output ${tojson(stageExplain)}`);
    assert(stageExplain.hasOwnProperty("spills"),
           `Did not find stage "spills" stat in explain output ${tojson(stageExplain)}`);
    assert(stageExplain.hasOwnProperty("spilledBytes"),
           `Did not find stage "spilledBytes" stat in explain output ${tojson(stageExplain)}`);
    assert(stageExplain.hasOwnProperty("spilledRecords"),
           `Did not find stage "spilledRecords" stat in explain output ${tojson(stageExplain)}`);
    assert(stageExplain.hasOwnProperty("spilledDataStorageSize"),
           `Did not find stage "spilledDataStorageSize" stat in explain output ${
               tojson(stageExplain)}`);
}

function testSpillingStats(pipeline, classicStageName, SBEStageName) {
    jsTest.log.info(`Running pipeline`, pipeline);

    const executionStats =
        db[collName].explain("executionStats").aggregate(pipeline, {"allowDiskUse": true});
    assertSpillingStats(executionStats, classicStageName, SBEStageName);

    const allPlansExecution =
        db[collName].explain("allPlansExecution").aggregate(pipeline, {"allowDiskUse": true});
    assertSpillingStats(allPlansExecution, classicStageName, SBEStageName);
}

testSpillingStats(pipelines['sort'], 'SORT', 'sort');      // classic, sbe
testSpillingStats(pipelines['group'], '$group', 'group');  // sbe, classic
testSpillingStats(
    pipelines['setWindowFields'], '$_internalSetWindowFields', 'window');  // classic, sbe
if (isSbeEnabled) {
    testSpillingStats(pipelines['lookup'], '$lookup', 'hash_lookup');
    testSpillingStats(pipelines['lookup-unwind'], '$lookup', 'hash_lookup_unwind');
}
testSpillingStats(pipelines['bucketAuto'], '$bucketAuto', '$bucketAuto');
testSpillingStats(pipelines['graphLookup'], '$graphLookup', '$graphLookup');

MongoRunner.stopMongod(conn);
