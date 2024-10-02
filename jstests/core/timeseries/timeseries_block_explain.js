/**
 * Tests the explain for time series block processing.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const sbeEnabled = checkSbeFullyEnabled(db) &&
    FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'TimeSeriesInSbe');

if (!sbeEnabled) {
    quit();
}

const coll = db.timeseries_bucket_level_filter;
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "time", metaField: "meta"}}));

// Bucket 1: a and b are both scalars.
assert.commandWorked(coll.insert({time: new Date(), meta: 1, a: 42, b: 17}));
assert.commandWorked(coll.insert({time: new Date(), meta: 1, a: 43, b: 18}));
assert.commandWorked(coll.insert({time: new Date(), meta: 1, a: 44, b: 19}));

// Bucket 2: a is an array, b is scalar.
assert.commandWorked(coll.insert({time: new Date(), meta: 1, a: [1, 2, 3], b: 17}));

// Bucket 3: a.b is an array.
assert.commandWorked(coll.insert({time: new Date(), meta: 1, a: {b: [1, 2, 3]}, b: 17}));

(function testBucketLevelFiltersOnCollScanPlan() {
    const pipeline = [{$match: {a: {$gt: 0}}}, {$project: {_id: 0, a: 1, b: 1}}];
    const explain = coll.explain("executionStats").aggregate(pipeline);

    // Ensure we get a collection scan, with one 'scan' stage.
    const scanStages = getSbePlanStages(explain, "scan");
    assert.eq(scanStages.length, 1, () => "Expected one scan stage " + tojson(explain));

    // Ensure the scan actually returned something.
    assert.gte(scanStages[0].nReturned,
               3,
               () => "Expected one value returned from scan " + tojson(explain));

    const bucketStages = getSbePlanStages(explain, "ts_bucket_to_cellblock");
    assert.eq(bucketStages.length, 1, () => "Expected one bucket stage " + tojson(explain));

    assert.eq(bucketStages[0].numCellBlocksProduced,
              9,  // 3 paths are used: Get(a)/Traverse/Id for $match and
                  // Get(a)/Id, Get(b)/Id for $project.
              () => "Expected 9 cellBlocks produced " + tojson(explain));
    assert.eq(bucketStages[0].numStorageBlocks,
              6,  // There are only storage blocks for 'a' and 'b'.
              () => "Expected 6 storage blocks " + tojson(explain));
    assert.eq(bucketStages[0].numStorageBlocksDecompressed,
              5,  // For the block where 'a' is an array with objects, the filter doesn't pass so
                  // we don't need to decompress 'b'.
              () => "Expected 5 storage blocks decoded " + tojson(explain));
})();
