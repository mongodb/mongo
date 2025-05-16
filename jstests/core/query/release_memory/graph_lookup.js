/**
 * Tests releaseMemory in graphLookup.
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
import {isTimeSeriesCollection} from "jstests/libs/cmd_object_utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {getEngine, hasMergeCursors} from "jstests/libs/query/analyze_plan.js";
import {
    accumulateServerStatusMetric,
    assertReleaseMemoryFailedWithCode
} from "jstests/libs/release_memory_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getSpillCounter() {
    return accumulateServerStatusMetric(db, metrics => metrics.query.graphLookup.spills);
}

const memoryLimitKnob = "internalDocumentSourceGraphLookupMaxMemoryBytes";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

function assertSortedArrayEq(actual, expected) {
    assert.eq(actual.length, expected.length);
    for (let i = 0; i < actual.length; ++i) {
        assert.eq(actual[i], expected[i]);
    }
}

const local = db[jsTestName() + "_local"];
const foreign = db[jsTestName() + "_foreign"];
assert(local.drop());
assert(foreign.drop());

assert.commandWorked(local.insertOne({start: 1}));

// Create a binary tree of depth 6 with 2^6 = 64 nodes.
// Each node has a payload of 1MB. It is needed for AsyncResultMerger not to fetch the whole result
// in a sharded cluster.
const docCount = 64;
const string1MB = Array(1024 * 1024).toString();
for (let i = 1; i <= docCount; ++i) {
    assert.commandWorked(
        foreign.insertOne({index: i, children: [2 * i, 2 * i + 1], payload: string1MB}));
}

const pipeline = [
    {
        $graphLookup: {
            from: foreign.getName(),
            startWith: "$start",
            connectFromField: "children",
            connectToField: "index",
            as: "output"
        }
    },
    {$unwind: {path: "$output"}},
    {$replaceRoot: {newRoot: "$output"}},
];

function sortPipelineResults(results) {
    results.forEach((e) => {
        delete e.payload;
    });
    return results.sort((a, b) => (a.index - b.index));
}

// Get all the results to use as a reference.
const expectedResults =
    sortPipelineResults(local.aggregate(pipeline, {"allowDiskUse": false}).toArray());

{
    jsTest.log(`Running releaseMemory after first batch`);
    let initialSpillCount = getSpillCounter();

    const cursor = local.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
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

    jsTest.log.info("Running getMore");
    const results = sortPipelineResults(cursor.toArray());
    assertSortedArrayEq(results, expectedResults);
    // $graphLookup updates server counters after the operation is finished, so we need to
    // check the spill count after the cursor is closed.
    assert.lt(initialSpillCount, getSpillCounter());
}

// Run query with tight memory limit to spill while creating the first batch.
{
    jsTest.log(`Running spill in first batch`);
    const originalMemoryLimitKnobValue = getServerParameter(memoryLimitKnob);
    setServerParameter(memoryLimitKnob, 1);
    let initialSpillCount = getSpillCounter();

    const cursor = local.aggregate(pipeline, {allowDiskUse: true, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
    const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
    assert.commandWorked(releaseMemoryRes);
    assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);

    jsTest.log.info("Running getMore");
    const results = sortPipelineResults(cursor.toArray());
    assertSortedArrayEq(results, expectedResults);

    assert.lt(initialSpillCount, getSpillCounter());
    setServerParameter(memoryLimitKnob, originalMemoryLimitKnobValue);
}

// Disallow spilling in graphLookup
{
    jsTest.log(`Running releaseMemory with no allowDiskUse`);

    const cursor = local.aggregate(pipeline, {"allowDiskUse": false, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
    const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
    assert.commandWorked(releaseMemoryRes);
    assertReleaseMemoryFailedWithCode(
        releaseMemoryRes, cursorId, ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    jsTest.log.info("Running getMore");
    const results = sortPipelineResults(cursor.toArray());
    assertSortedArrayEq(results, expectedResults);
}
