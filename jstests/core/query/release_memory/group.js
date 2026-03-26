/**
 * Tests releaseMemory in groupBy.
 *
 * @tags: [
 *   requires_fcv_82,
 *   # We use explain and we modify the value of a query knob using setParameter.
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   # This test sets a server parameter via setParameterOnAllNonConfigNodes. To keep the
 *   # host list consistent, no add/remove shard operations should occur during the test
 *   # and the read preference should not change.
 *   assumes_stable_shard_list,
 *   assumes_read_preference_unchanged,
 *   # This test relies on aggregations returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 *   # This test holds open cursors across releaseMemory and getMore calls. Random sharding
 *   # metadata clears can invalidate those cursors mid-flight with StaleConfig errors.
 *   incompatible_with_random_sharding_metadata_clears,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {isTimeSeriesCollection} from "jstests/libs/cmd_object_utils.js";
import {getEngine, hasMergeCursors} from "jstests/libs/query/analyze_plan.js";
import {
    accumulateServerStatusMetric,
    assertReleaseMemoryFailedWithCode,
    runReleaseMemoryTestWithRetries,
    setAvailableDiskSpaceMode,
} from "jstests/libs/release_memory_util.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getSpillCounter() {
    return accumulateServerStatusMetric(db, (metrics) => metrics.query.group.spills);
}

const sbeIncreasedSpillingKnob = "internalQuerySlotBasedExecutionHashAggIncreasedSpilling";
const classicIncreasedSpillingKnob = "internalQueryEnableAggressiveSpillsInGroup";
const sbeMemorySizeKnob = "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill";
const classicMemorySizeKnob = "internalDocumentSourceGroupMaxMemoryBytes";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    setParameterOnAllNonConfigNodes(db.getMongo(), knob, value);
}

const sbeIncreasedSpillingInitialValue = getServerParameter(sbeIncreasedSpillingKnob);
const classicIncreasedSpillingInitialValue = getServerParameter(classicIncreasedSpillingKnob);
const sbeMemorySizeInitialValue = getServerParameter(sbeMemorySizeKnob);
const classicMemorySizeInitialValue = getServerParameter(classicMemorySizeKnob);

// We want to control spilling. Disable increased spilling.
setServerParameter(sbeIncreasedSpillingKnob, "never");
setServerParameter(classicIncreasedSpillingKnob, false);

// For _internalInhibitOptimization tests, prevent DSCursor from reading all the data in a single
// batch.
const dsCursorKnobs = ["internalDocumentSourceCursorInitialBatchSize", "internalDocumentSourceCursorBatchSizeBytes"];
const dsCursorKnobNewValues = [1, 1024];
const dsCursorKnobValues = [];
for (let i = 0; i < dsCursorKnobs.length; i++) {
    dsCursorKnobValues.push(getServerParameter(dsCursorKnobs[i]));
    setServerParameter(dsCursorKnobs[i], dsCursorKnobNewValues[i]);
}

const collFew = db[jsTestName() + "_few"];
assert(collFew.drop());
const collMany = db[jsTestName() + "_many"];
assert(collMany.drop());

const fewDocs = [
    {"_id": 1, "item": "a", "price": 10, "quantity": 2, "date": ISODate("2014-01-01T08:00:00Z")},
    {"_id": 2, "item": "b", "price": 20, "quantity": 1, "date": ISODate("2014-02-03T09:00:00Z")},
    {"_id": 3, "item": "a", "price": 5, "quantity": 5, "date": ISODate("2014-02-03T09:05:00Z")},
    {"_id": 4, "item": "b", "price": 10, "quantity": 10, "date": ISODate("2014-02-15T08:00:00Z")},
    {"_id": 5, "item": "c", "price": 5, "quantity": 10, "date": ISODate("2014-02-15T09:05:00Z")},
    {"_id": 6, "item": "c", "price": 15, "quantity": 3, "date": ISODate("2014-03-01T10:00:00Z")},
    {"_id": 7, "item": "d", "price": 30, "quantity": 1, "date": ISODate("2014-03-12T08:00:00Z")},
    {"_id": 8, "item": "e", "price": 25, "quantity": 4, "date": ISODate("2014-03-21T11:00:00Z")},
    {"_id": 9, "item": "f", "price": 10, "quantity": 6, "date": ISODate("2014-04-01T12:00:00Z")},
    {"_id": 10, "item": "g", "price": 50, "quantity": 2, "date": ISODate("2014-04-07T13:00:00Z")},
    {"_id": 11, "item": "a", "price": 10, "quantity": 8, "date": ISODate("2014-04-15T10:00:00Z")},
    {"_id": 12, "item": "b", "price": 20, "quantity": 5, "date": ISODate("2014-04-15T09:30:00Z")},
    {"_id": 13, "item": "h", "price": 5, "quantity": 15, "date": ISODate("2014-04-20T14:00:00Z")},
    {"_id": 14, "item": "i", "price": 40, "quantity": 7, "date": ISODate("2014-04-26T15:00:00Z")},
    {"_id": 15, "item": "j", "price": 35, "quantity": 1, "date": ISODate("2014-05-02T16:00:00Z")},
];
assert.commandWorked(collFew.insertMany(fewDocs));

let manyDocs = [];
for (let i = 0; i <= 200; i++) {
    // Create item key with some duplicates:
    // - 80% of documents have unique items (128+ distinct items)
    // - 20% of documents reuse existing items to test grouping behavior
    const itemChar =
        i % 5 === 0
            ? String.fromCharCode(96 + (i % 26) + 1) // Creates duplicates from a-z
            : String.fromCharCode(96 + (i % 220) + 1); // Still ensures 200+ distinct items

    manyDocs.push({
        "_id": i,
        "item": itemChar,
        "price": Math.floor(Math.random() * 100),
        "quantity": Math.floor(Math.random() * 20),
        "date": new Date(2014, Math.floor(i % 12), Math.floor((i % 28) + 1)),
    });
}
assert.commandWorked(collMany.insertMany(manyDocs));

const groupPipeline = [
    {
        $group: {
            _id: "$item",
            totalQuantity: {$sum: "$quantity"},
            totalRevenue: {$sum: {$multiply: ["$price", "$quantity"]}},
        },
    },
];

const pipelines = [
    groupPipeline,
    groupPipeline.concat({$_internalInhibitOptimization: {}}), // Prevents the pipeline from being eliminated.
];

const collections = [collFew, collMany];

// Get explain and the expected results to use later for verification.
// We want to get the explain before running the tests to make sure that
// the explain is not affected by the releaseMemory command, which might
// cause additional spilling.
const pipelineData = [];
for (let pipelineIndex = 0; pipelineIndex < pipelines.length; pipelineIndex++) {
    const pipeline = pipelines[pipelineIndex];
    const dataForPipeline = [];
    for (let collIndex = 0; collIndex < collections.length; collIndex++) {
        const coll = collections[collIndex];
        const explain = coll.explain().aggregate(pipeline);

        // Get all the results to use as a reference. Set 'allowDiskUse' to false to disable spilling.
        const expectedResults = coll.aggregate(pipeline, {"allowDiskUse": false}).toArray();

        dataForPipeline[collIndex] = {
            explain,
            expectedResults,
        };
    }
    pipelineData[pipelineIndex] = dataForPipeline;
}

// To minimise the setServerParameter calls that overload the config server,
// we run every test case for all pipelines and collections together, then
// move on to the next test case.

// No disk space available for spilling.
jsTest.log.info(`Running \`releaseMemory with no disk space available\` test case`);
setAvailableDiskSpaceMode(db.getSiblingDB("admin"), "alwaysOn");
for (let pipelineIndex = 0; pipelineIndex < pipelines.length; pipelineIndex++) {
    const pipeline = pipelines[pipelineIndex];
    for (let collIndex = 0; collIndex < collections.length; collIndex++) {
        const coll = collections[collIndex];
        const {explain, expectedResults} = pipelineData[pipelineIndex][collIndex];

        const willNotSpill =
            isTimeSeriesCollection(db, coll.getName()) && expectedResults.length <= 128 && getEngine(explain) === "sbe";
        if (willNotSpill) {
            continue;
        }
        jsTest.log.info(`Testing collection ${coll.getName()} on pipeline: ${tojson(pipeline)}`);
        const cursor = coll.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
        const cursorId = cursor.getId();

        // Release memory (i.e., spill)
        const releaseMemoryCmd = {releaseMemory: [cursorId]};
        jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
        const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
        assert.commandWorked(releaseMemoryRes);
        assertReleaseMemoryFailedWithCode(releaseMemoryRes, cursorId, ErrorCodes.OutOfDiskSpace);

        jsTest.log.info("Running getMore");
        assert.throwsWithCode(() => cursor.toArray(), ErrorCodes.CursorNotFound);
    }
}
setAvailableDiskSpaceMode(db.getSiblingDB("admin"), "off");

// Disallow spilling in group.
jsTest.log.info(`Running \`releaseMemory with no allowDiskUse\` test case`);
for (let pipelineIndex = 0; pipelineIndex < pipelines.length; pipelineIndex++) {
    const pipeline = pipelines[pipelineIndex];
    for (let collIndex = 0; collIndex < collections.length; collIndex++) {
        const coll = collections[collIndex];
        const {explain, expectedResults} = pipelineData[pipelineIndex][collIndex];

        if (hasMergeCursors(explain)) {
            // When `allowDiskUse` is false and a pipeline with `$mergeCursors` is used, operations
            // might execute in `mongos`. So, the group operation will be performed on `mongos`, and
            // `forceSpill` will be disregarded.
            continue;
        }
        jsTest.log.info(`Testing collection ${coll.getName()} on pipeline: ${tojson(pipeline)}`);
        const cursor = coll.aggregate(pipeline, {"allowDiskUse": false, cursor: {batchSize: 1}});
        const cursorId = cursor.getId();

        // Release memory (i.e., spill)
        const releaseMemoryCmd = {releaseMemory: [cursorId]};
        jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
        const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
        assert.commandWorked(releaseMemoryRes);
        assertReleaseMemoryFailedWithCode(releaseMemoryRes, cursorId, [
            ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
            ErrorCodes.ReleaseMemoryShardError,
        ]);

        jsTest.log.info("Running getMore");
        const results = cursor.toArray();
        assertArrayEq({actual: results, expected: expectedResults});
    }
}

jsTest.log.info(`Setting memory to 100MB and running \`no spill in first batch\` test case`);
setServerParameter(sbeMemorySizeKnob, 100 * 1024 * 1024);
setServerParameter(classicMemorySizeKnob, 100 * 1024 * 1024);
for (let pipelineIndex = 0; pipelineIndex < pipelines.length; pipelineIndex++) {
    const pipeline = pipelines[pipelineIndex];
    for (let collIndex = 0; collIndex < collections.length; collIndex++) {
        const coll = collections[collIndex];
        const {explain, expectedResults} = pipelineData[pipelineIndex][collIndex];

        jsTest.log.info(`Testing collection ${coll.getName()} on pipeline: ${tojson(pipeline)}`);
        runReleaseMemoryTestWithRetries(() => {
            let initialSpillCount = getSpillCounter();

            const cursor = coll.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
            const cursorId = cursor.getId();
            const newSpillCount = getSpillCounter();
            assert.eq(newSpillCount, initialSpillCount);
            initialSpillCount = newSpillCount;

            // Release memory (i.e., spill)
            const releaseMemoryCmd = {releaseMemory: [cursorId]};
            jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
            const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
            assert.commandWorked(releaseMemoryRes);
            assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
            // In a timeseries collection, in sbe, all records are consumed in the first batch (up
            // to kBlockOutSize = 128) and there is nothing to spill.
            const willNotSpill =
                isTimeSeriesCollection(db, coll.getName()) &&
                expectedResults.length <= 128 &&
                getEngine(explain) === "sbe";
            if (willNotSpill) {
                assert.eq(initialSpillCount, getSpillCounter());
            } else {
                assert.lt(initialSpillCount, getSpillCounter());
            }

            jsTest.log.info("Running getMore");
            const results = cursor.toArray();
            assertArrayEq({actual: results, expected: expectedResults});
        });
    }
}

jsTest.log.info(`Leaving available memory to 100MB and running \`return all results in the first batch\` test case`);
for (let pipelineIndex = 0; pipelineIndex < pipelines.length; pipelineIndex++) {
    const pipeline = pipelines[pipelineIndex];
    for (let collIndex = 0; collIndex < collections.length; collIndex++) {
        const coll = collections[collIndex];
        const {explain, expectedResults} = pipelineData[pipelineIndex][collIndex];

        jsTest.log.info(`Testing collection ${coll.getName()} on pipeline: ${tojson(pipeline)}`);
        runReleaseMemoryTestWithRetries(() => {
            let initialSpillCount = getSpillCounter();

            const cursor = coll.aggregate(pipeline, {
                "allowDiskUse": true,
                cursor: {batchSize: expectedResults.length},
            });
            const cursorId = cursor.getId();
            const newSpillCount = getSpillCounter();
            assert.eq(newSpillCount, initialSpillCount);
            initialSpillCount = newSpillCount;

            // Release memory (i.e., spill)
            const releaseMemoryCmd = {releaseMemory: [cursorId]};
            jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
            const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
            assert.commandWorked(releaseMemoryRes);
            assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
            assert.eq(initialSpillCount, getSpillCounter());

            jsTest.log.info("Running getMore");
            const results = cursor.toArray();
            assertArrayEq({actual: results, expected: expectedResults});
        });
    }
}

// Run query with increased spilling to spill while creating the first batch.
jsTest.log.info(`Setting memory to 1KB and running \`spill in first batch\` test case`);
setServerParameter(sbeMemorySizeKnob, 1 * 1024);
setServerParameter(classicMemorySizeKnob, 1 * 1024);
for (let pipelineIndex = 0; pipelineIndex < pipelines.length; pipelineIndex++) {
    const pipeline = pipelines[pipelineIndex];
    for (let collIndex = 0; collIndex < collections.length; collIndex++) {
        const coll = collections[collIndex];
        const {explain, expectedResults} = pipelineData[pipelineIndex][collIndex];

        jsTest.log.info(`Testing collection ${coll.getName()} on pipeline: ${tojson(pipeline)}`);
        runReleaseMemoryTestWithRetries(() => {
            let initialSpillCount = getSpillCounter();

            const cursor = coll.aggregate(pipeline, {allowDiskUse: true, cursor: {batchSize: 1}});
            const cursorId = cursor.getId();
            const newSpillCount = getSpillCounter();
            assert.lt(initialSpillCount, newSpillCount);
            initialSpillCount = newSpillCount;

            // Release memory (i.e., spill)
            const releaseMemoryCmd = {releaseMemory: [cursorId]};
            jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
            const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
            assert.commandWorked(releaseMemoryRes);
            assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
            assert.eq(initialSpillCount, newSpillCount);

            jsTest.log.info("Running getMore");
            const results = cursor.toArray();
            assertArrayEq({actual: results, expected: expectedResults});
        });
    }
}

jsTest.log.info(`Restoring server parameters to initial values`);
setServerParameter(sbeMemorySizeKnob, sbeMemorySizeInitialValue);
setServerParameter(classicMemorySizeKnob, classicMemorySizeInitialValue);
setServerParameter(sbeIncreasedSpillingKnob, sbeIncreasedSpillingInitialValue);
setServerParameter(classicIncreasedSpillingKnob, classicIncreasedSpillingInitialValue);
for (let i = 0; i < dsCursorKnobs.length; i++) {
    setServerParameter(dsCursorKnobs[i], dsCursorKnobValues[i]);
}
