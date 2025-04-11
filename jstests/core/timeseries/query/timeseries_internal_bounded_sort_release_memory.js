/**
 * Test releaseMemory command for cursors with $_internalBoundedSort stage.
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   assumes_superuser_permissions,
 *   does_not_support_transactions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_82,
 *   requires_getmore,
 *   requires_timeseries,
 *   uses_getmore_outside_of_transaction,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {
    accumulateServerStatusMetric,
    assertReleaseMemoryFailedWithCode
} from "jstests/libs/release_memory_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

function getSortSpillCounter() {
    return accumulateServerStatusMetric(db, metrics => metrics.query.sort.spillToDisk);
}

const sortMemoryLimitKnob = "internalQueryMaxBlockingSortMemoryUsageBytes";

db.dropDatabase();
const coll = db[jsTestName()];
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
const bucketMaxSpanSeconds =
    db.getCollectionInfos({name: coll.getName()})[0].options.timeseries.bucketMaxSpanSeconds;

const start = new Date();
const kMetaCount = 10;
const kDocCount = 100;

const string1KB = 'X'.repeat(1024);
for (let i = 0; i < kMetaCount; ++i) {
    const batch = [];
    let batchTime = +start + i;
    for (let j = 0; j < kDocCount; ++j) {
        batch.push({time: new Date(batchTime), meta: i, padding: string1KB});
        batchTime += bucketMaxSpanSeconds / 10;
    }
    assert.commandWorked(coll.insertMany(batch));
}

function assertCursorSortedByTime(cursor) {
    let previousTime;
    for (let i = 0; i < kMetaCount * kDocCount; ++i) {
        const doc = cursor.next();
        if (previousTime) {
            assert.lte(previousTime, doc.time);
        }
        previousTime = doc.time;
    }
    assert.eq(cursor.hasNext(), false);
}

// Some background queries can use $group and classic $group uses sorter to spill, so this
// background spills can affect server status metrics.
const classicGroupIncreasedSpillingKnob = "internalQueryEnableAggressiveSpillsInGroup";
const classicGroupIncreasedSpillingInitialValue =
    getServerParameter(classicGroupIncreasedSpillingKnob);
setServerParameter(classicGroupIncreasedSpillingKnob, false);

const pipeline = [{$sort: {time: 1}}];

const explain = coll.explain().aggregate(pipeline);
const stages = getAggPlanStages(explain, "$_internalBoundedSort");
assert.neq(null, stages, explain);
assert.lte(1, stages.length, explain);

jsTestLog("Testing pipeline: " + tojson(pipeline));

let previousSpillCount = getSortSpillCounter();
assertCursorSortedByTime(coll.aggregate(pipeline));
assert.eq(previousSpillCount, getSortSpillCounter());

{  // Release memory should spill, which will increment spill serverStatus counters.
    const cursor = coll.aggregate(pipeline, {cursor: {batchSize: 1}});
    const cursorId = cursor.getId();
    assert.eq(previousSpillCount, getSortSpillCounter());

    const releaseMemoryRes = db.runCommand({releaseMemory: [cursorId]});
    assert.commandWorked(releaseMemoryRes);
    assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
    assert.lt(previousSpillCount, getSortSpillCounter());
    previousSpillCount = getSortSpillCounter();

    assertCursorSortedByTime(cursor);
}

{  // Cursor with allowDiskUse: false should be reported as failed.
    const cursor = coll.aggregate(pipeline, {cursor: {batchSize: 1}, allowDiskUse: false});
    const cursorId = cursor.getId();

    const releaseMemoryRes = db.runCommand({releaseMemory: [cursorId]});
    assert.commandWorked(releaseMemoryRes);
    assertReleaseMemoryFailedWithCode(
        releaseMemoryRes, cursorId, ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    assertCursorSortedByTime(cursor);
}

{  // Release memory should work when the cursor already have spilled by itself.
    const originalKnobValue = getServerParameter(sortMemoryLimitKnob);
    setServerParameter(sortMemoryLimitKnob, 10 * 1024);

    const cursor = coll.aggregate(pipeline, {cursor: {batchSize: 1}});
    const cursorId = cursor.getId();
    assert.lt(previousSpillCount, getSortSpillCounter());
    previousSpillCount = getSortSpillCounter();

    const releaseMemoryRes = db.runCommand({releaseMemory: [cursorId]});
    assert.commandWorked(releaseMemoryRes);
    assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
    assert.lt(previousSpillCount, getSortSpillCounter());

    assertCursorSortedByTime(cursor);
    setServerParameter(sortMemoryLimitKnob, originalKnobValue);
}

setServerParameter(classicGroupIncreasedSpillingKnob, classicGroupIncreasedSpillingInitialValue);
