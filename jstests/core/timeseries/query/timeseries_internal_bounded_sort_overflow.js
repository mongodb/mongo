/**
 * Reproducer for an integer overflow bug in $_internalBoundedSort.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
const unpackStage = getAggPlanStage(coll.explain().aggregate(), "$_internalUnpackBucket");
assert(unpackStage.$_internalUnpackBucket);

// Insert some data: two events far enough apart that their difference in ms can overflow an int.
const docs = [{t: ISODate("2000-01-01T00:00:00Z")}, {t: ISODate("2000-02-01T00:00:00Z")}];
assert.commandWorked(coll.insert(docs));

// Make sure $_internalBoundedSort accepts it.
const result = getTimeseriesCollForRawOps(coll)
    .aggregate(
        [{$sort: {"control.min.t": 1}}, unpackStage, {$_internalBoundedSort: {sortKey: {t: 1}, bound: {base: "min"}}}],
        kRawOperationSpec,
    )
    .toArray();

// Make sure the result is in order.
assert.eq(result[0].t, docs[0].t);
assert.eq(result[1].t, docs[1].t);
