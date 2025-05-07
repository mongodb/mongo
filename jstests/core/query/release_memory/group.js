/**
 * Tests releaseMemory in groupBy.
 *
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
 *   # This test relies on aggregations returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {hasMergeCursors} from "jstests/libs/query/analyze_plan.js";
import {
    accumulateServerStatusMetric,
    assertReleaseMemoryFailedWithCode
} from "jstests/libs/release_memory_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getSpillCounter() {
    return accumulateServerStatusMetric(db, metrics => metrics.query.group.spills);
}

const sbeIncreasedSpillingKnob = "internalQuerySlotBasedExecutionHashAggIncreasedSpilling";
const classicIncreasedSpillingKnob = "internalQueryEnableAggressiveSpillsInGroup";
const sbeMemorySizeKnob = "internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill";
const classicMemorySizeKnob = "internalDocumentSourceGroupMaxMemoryBytes";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

const sbeIncreasedSpillingInitialValue = getServerParameter(sbeIncreasedSpillingKnob);
const classicIncreasedSpillingInitialValue = getServerParameter(classicIncreasedSpillingKnob);
const sbeMemorySizeInitialValue = getServerParameter(sbeMemorySizeKnob);
const classicMemorySizeInitialValue = getServerParameter(classicMemorySizeKnob);

// We want to control spilling. Disable increased spilling.
setServerParameter(sbeIncreasedSpillingKnob, "never");
setServerParameter(classicIncreasedSpillingKnob, false);

const coll = db[jsTestName()];
assert(coll.drop());

const docs = [
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
assert.commandWorked(coll.insertMany(docs));

const pipeline = [{
    $group: {
        _id: "$item",
        totalQuantity: {$sum: "$quantity"},
        totalRevenue: {$sum: {$multiply: ["$price", "$quantity"]}}
    }
}];

// ToDo: Remove the mergeCursors on SERVER-104522
const explain = coll.explain().aggregate(pipeline);
if (hasMergeCursors(explain)) {
    jsTest.log(`Skipping test. Pipeline has $mergeCursors but spilling on mongos is not allowed`);
    quit();
}

// Get all the results to use as a reference. Set 'allowDiskUse' to false to disable increased
// spilling in debug builds.
const expectedResults = coll.aggregate(pipeline, {"allowDiskUse": false}).toArray();

{
    jsTest.log(`Running no spill in first batch`);

    setServerParameter(sbeMemorySizeKnob, 100 * 1024 * 1024);
    setServerParameter(classicMemorySizeKnob, 100 * 1024 * 1024);
    let initialSpillCount = getSpillCounter();

    // Retrieve the first batch without spilling.
    jsTest.log.info("Running pipeline: ", pipeline[0]);

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
    assert.lt(initialSpillCount, getSpillCounter());

    jsTest.log.info("Running getMore");
    const results = cursor.toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    setServerParameter(sbeMemorySizeKnob, sbeMemorySizeInitialValue);
    setServerParameter(classicMemorySizeKnob, classicMemorySizeInitialValue);
}

// Run query with increased spilling to spill while creating the first batch.
{
    jsTest.log(`Running spill in first batch`);
    setServerParameter(sbeMemorySizeKnob, 1);
    setServerParameter(classicMemorySizeKnob, 1);
    let initialSpillCount = getSpillCounter();

    // Retrieve the first batch.
    jsTest.log.info("Running pipeline: ", pipeline[0]);

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

    setServerParameter(sbeMemorySizeKnob, sbeMemorySizeInitialValue);
    setServerParameter(classicMemorySizeKnob, classicMemorySizeInitialValue);
}

// Disallow spilling in group
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
        releaseMemoryRes, cursorId, ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    jsTest.log.info("Running getMore");
    const results = cursor.toArray();
    assertArrayEq({actual: results, expected: expectedResults});
}

setServerParameter(sbeIncreasedSpillingKnob, sbeIncreasedSpillingInitialValue);
setServerParameter(classicIncreasedSpillingKnob, classicIncreasedSpillingInitialValue);
