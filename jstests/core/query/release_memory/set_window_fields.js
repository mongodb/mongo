/**
+ * Tests releaseMemory in setWindowFields.
+ *
 * @tags: [
 *   requires_fcv_82,
 *   # We modify the value of a query knob. setParameter is not persistent.
 *   does_not_support_stepdowns,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   assumes_read_preference_unchanged,
 *   does_not_support_transactions,
 *   # releaseMemory needs special permission
 *   assumes_superuser_permissions,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {hasMergeCursors} from "jstests/libs/query/analyze_plan.js";
import {
    accumulateServerStatusMetric,
    assertReleaseMemoryFailedWithCode,
    setAvailableDiskSpaceMode
} from "jstests/libs/release_memory_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getSpillCounter() {
    return accumulateServerStatusMetric(db, metrics => metrics.query.setWindowFields.spills);
}

const memoryKnob = "internalDocumentSourceSetWindowFieldsMaxMemoryBytes";
function getServerParameter() {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [memoryKnob]: 1}))[memoryKnob];
}
function setServerParameter(value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), memoryKnob, value);
}
const memoryInitialValue = getServerParameter();

const coll = db[jsTestName()];
coll.drop();

const docs = [
    {"_id": 1, "category": "Electronics", "amount": 100, "date": "2023-01-01"},
    {"_id": 2, "category": "Electronics", "amount": 200, "date": "2023-01-02"},
    {"_id": 3, "category": "Furniture", "amount": 150, "date": "2023-01-01"},
    {"_id": 4, "category": "Electronics", "amount": 50, "date": "2023-01-03"},
    {"_id": 5, "category": "Furniture", "amount": 300, "date": "2023-01-02"},
];
assert.commandWorked(coll.insertMany(docs));

const pipeline = [{
    $setWindowFields: {
        partitionBy: "$category",
        sortBy: {date: 1},
        output: {runningTotal: {$sum: "$amount", window: {documents: ["unbounded", "current"]}}}
    }
}];

// Get all the results to use as a reference.
const expectedResults = coll.aggregate(pipeline, {"allowDiskUse": false}).toArray();

{
    jsTest.log(`Running no spill in first batch`);
    setServerParameter(100 * 1024 * 1024);

    // Retrieve the first batch without spilling.
    jsTest.log.info("Running pipeline: ", pipeline[0]);
    const cursor = coll.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    // Release memory (i.e., spill)
    const initialSpillCount = getSpillCounter();

    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
    const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
    assert.commandWorked(releaseMemoryRes);
    assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);

    jsTest.log.info("Running getMore");
    const results = cursor.toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // Check that the spill counter is incremented. Classic $setWindowFields updated counters only
    // after the query was completed.
    assert.lt(initialSpillCount, getSpillCounter());

    setServerParameter(memoryInitialValue);
}

// Run query with increased spilling to spill while creating the first batch.
{
    jsTest.log(`Running spill in first batch`);
    setServerParameter(1024);

    // Retrieve the first batch.
    jsTest.log.info("Running pipeline: ", pipeline[0]);
    const cursor = coll.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    // Release memory (i.e., spill)
    const initialSpillCount = getSpillCounter();

    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
    const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
    assert.commandWorked(releaseMemoryRes);
    assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);

    jsTest.log.info("Running getMore");
    const results = cursor.toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // Check that the spill counter is incremented. Classic $setWindowFields updated counters only
    // after the query was completed.
    assert.lt(initialSpillCount, getSpillCounter());

    setServerParameter(memoryInitialValue);
}

// No disk space available for spilling.
{
    jsTest.log(`Running releaseMemory with no disk space available`);
    const cursor = coll.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    // Release memory (i.e., spill)
    setAvailableDiskSpaceMode(db.getSiblingDB("admin"), 'alwaysOn');
    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
    const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
    assert.commandWorked(releaseMemoryRes);
    assertReleaseMemoryFailedWithCode(releaseMemoryRes, cursorId, ErrorCodes.OutOfDiskSpace);
    setAvailableDiskSpaceMode(db.getSiblingDB("admin"), 'off');

    jsTest.log.info("Running getMore");
    assert.throwsWithCode(() => cursor.toArray(), ErrorCodes.CursorNotFound);
}

const explain = coll.explain().aggregate(pipeline);
if (hasMergeCursors(explain)) {
    // When `allowDiskUse` is false and a pipeline with `$mergeCursors` is used, operations
    // might execute in `mongos`. So, the setWindowFields operation will be performed on `mongos`,
    // and `forceSpill` will be disregarded.
    quit();
}

// Disallow spilling in setWindowFields.
{
    jsTest.log(`Running releaseMemory with no allowDiskUse`);

    // Retrieve the first batch without spilling.
    jsTest.log.info("Running pipeline: ", pipeline[0]);
    const cursor = coll.aggregate(pipeline, {"allowDiskUse": false, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    // Release memory (i.e., spill)
    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
    const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
    assert.commandWorked(releaseMemoryRes);
    assertReleaseMemoryFailedWithCode(
        releaseMemoryRes, cursorId, [ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed, 5643008]);

    jsTest.log.info("Running getMore");
    const results = cursor.toArray();
    assertArrayEq({actual: results, expected: expectedResults});
}
