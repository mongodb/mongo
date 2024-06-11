/**
 * Tests that internalQuerySlotBasedExecutionDisableTimeSeriesPushdown query knob correctly disables
 * time-series queries running in SBE.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   requires_fcv_72
 * ]
 */

import {getAggPlanStage} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const dbName = jsTestName();
const db = conn.getDB(dbName);

// We pushdown unpack when checkSbeRestrictedOrFullyEnabled is true and when
// featureFlagTimeSeriesInSbe is set.
const sbeEnabled = checkSbeRestrictedOrFullyEnabled(db) &&
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'TimeSeriesInSbe');

const coll = db.timeseries;
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
// The dataset doesn't matter, as we only care about the choice of the plan to execute the query.
assert.commandWorked(coll.insert({t: new Date(), m: 1, a: 42, b: 17}));

const pipeline = [{$match: {t: {$lt: new Date()}}}, {$project: {_id: 1, t: 1}}];

function runTest(expectedUnpackStage) {
    const expectedStage = sbeEnabled ? expectedUnpackStage : "$_internalUnpackBucket";
    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    assert.neq(null,
               getAggPlanStage(explain, expectedStage),
               `Should execute unpack in ${expectedStage}, but ran: ${tojson(explain)}.`);
}

// The default value of the query knob is false. The unpack stage should be lowered to SBE.
runTest("UNPACK_TS_BUCKET");

// Set the query knob to true. The unpack stage should not be lowered to SBE.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionDisableTimeSeriesPushdown: true}));
runTest("$_internalUnpackBucket");

// Set the query knob back to false. The unpack stage should be lowered to SBE.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionDisableTimeSeriesPushdown: false}));
runTest("UNPACK_TS_BUCKET");

MongoRunner.stopMongod(conn);
