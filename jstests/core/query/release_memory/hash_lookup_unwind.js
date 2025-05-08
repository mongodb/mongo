/**
+ * Tests releaseMemory in hash lookup.
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
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {getAggPlanStages, getEngine} from "jstests/libs/query/analyze_plan.js";
import {accumulateServerStatusMetric} from "jstests/libs/release_memory_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getSpillCounter() {
    return accumulateServerStatusMetric(db, metrics => metrics.query.lookup.hashLookupSpills);
}

const memoryKnob = "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill";
const sbeIncreasedSpillingKnob = "internalQuerySlotBasedExecutionHashAggIncreasedSpilling";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}
function setServerParameter(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

const memoryInitialValue = getServerParameter(memoryKnob);
const sbeIncreasedSpillingInitialValue = getServerParameter(sbeIncreasedSpillingKnob);

// HashLookup in SBE might use HashAgg. We want to control spilling. Disable increased spilling.
setServerParameter(sbeIncreasedSpillingKnob, "never");

// 'locations' is used as the foreign collection for $lookup.
const locations = db[jsTestName() + "_locations"];
locations.drop();
// 'animals' is used as the local collection for $lookup.
const animals = db[jsTestName() + "_animals"];
animals.drop();

const locationsDocs = [
    {
        name: "doghouse",
        coordinates: [25.0, 60.0],
        extra: {breeds: ["terrier", "dachshund", "bulldog"]}
    },
    {
        name: "bullpen",
        coordinates: [-25.0, -60.0],
        extra: {breeds: "Scottish Highland", feeling: "bullish"}
    },
    {name: "volcano", coordinates: [-1111.0, 2222.0], extra: {breeds: "basalt", feeling: "hot"}}
];
const animasDocs = [
    {type: "dog", locationName: "doghouse", colors: ["chartreuse", "taupe", "green"]},
    {type: "bull", locationName: "bullpen", colors: ["red", "blue"]},
    {type: "trout", locationName: ["mauve"]},
];

assert.commandWorked(locations.insertMany(locationsDocs));
assert.commandWorked(animals.insertMany(animasDocs));

const pipeline = [
    {
        $lookup: {from: locations.getName(), localField: "locationName", foreignField: "name", as: "location"}
    },
    {$unwind: "$location"},
    {
        $project: {
            locationName: false,
            "location.extra": false,
            "location.coordinates": false,
            "colors": false
        }
    },
];

jsTest.log.info("Running pipeline: ", pipeline);

const explain = animals.explain().aggregate(pipeline);
if (getEngine(explain) === "classic") {
    jsTest.log.info("Skipping test, $lookup does not spill in classic", explain);
    quit();
}
const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP_UNWIND");
if (eqLookupNodes.length === 0) {
    jsTest.log.info("Skipping test, $lookup-$unwind has not been pushed to SBE", explain);
    quit();
}

// Get all the results to use as a reference. Set 'allowDiskUse' to false to disable
// increased spilling in debug builds.
const expectedResults = animals.aggregate(pipeline, {"allowDiskUse": false}).toArray();
jsTest.log.info("Expected results: ", expectedResults);
{
    jsTest.log(`Running no spill in first batch`);
    setServerParameter(memoryKnob, 100 * 1024 * 1024);
    let initialSpillCount = getSpillCounter();

    // Retrieve the first batch without spilling.
    const cursor = animals.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    // Assert it did not spill during the first batch.
    const newSpillCount = getSpillCounter();
    assert.eq(initialSpillCount, newSpillCount);
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

    setServerParameter(memoryKnob, memoryInitialValue);
}

// Run query with increased spilling to spill while creating the first batch.
{
    jsTest.log(`Running spill in first batch`);
    setServerParameter(memoryKnob, 1);
    let initialSpillCount = getSpillCounter();

    // Retrieve the first batch.
    const cursor = animals.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    // Assert it spilt during the first batch.
    const newSpillCount = getSpillCounter();
    assert.lt(initialSpillCount, newSpillCount);
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

    setServerParameter(memoryKnob, memoryInitialValue);
}

setServerParameter(sbeIncreasedSpillingKnob, sbeIncreasedSpillingInitialValue);
