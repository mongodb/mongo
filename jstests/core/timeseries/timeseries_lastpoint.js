/**
 * Tests the optimization of "lastpoint"-type queries on time-series collections.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Testing last point optimization.
 *   requires_pipeline_optimization,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/core/timeseries/libs/timeseries_agg_helpers.js");
load("jstests/core/timeseries/libs/timeseries_lastpoint_helpers.js");
load("jstests/libs/analyze_plan.js");

const testDB = TimeseriesAggTests.getTestDb();
assert.commandWorked(testDB.dropDatabase());

// Do not run the rest of the tests if the lastpoint optimization is disabled.
if (!FeatureFlagUtil.isEnabled(db, "LastPointQuery")) {
    return;
}

/**
 * Returns a lastpoint $group stage of the form:
 *  {$group: {
 *       _id: "$tags.hostid",
 *       usage_user: {$first: "$usage_user"}, ...
 *  }}
 */
function getGroupStage(time, extraFields = []) {
    const accumulator = time > 0 ? "$last" : "$first";
    let innerGroup = {_id: "$tags.hostid"};
    for (const f of extraFields.concat(["usage_user", "usage_guest", "usage_idle"])) {
        innerGroup[f] = {[accumulator]: "$" + f};
    }
    return {$group: innerGroup};
}

{
    const [tsColl, observerColl] = createBoringCollections();
    testAllTimeMetaDirections(
        tsColl, observerColl, ({time, canUseDistinct, canSortOnTimeUseDistinct, index}) => {
            const groupStage = getGroupStage(time);
            const expectCollscanNoSort = ({explain}) =>
                expectCollScan({explain, noSortInCursor: true});
            const getTestWithMatch = (matchStage, precedingFilter) => {
                return {
                    precedingFilter,
                    pipeline: [matchStage, {$sort: index}, groupStage],
                    expectStageWithIndex: (canUseDistinct ? expectDistinctScan : expectIxscan),
                };
            };

            return [
                // Test pipeline without a preceding $match stage with sort only on time.
                {
                    pipeline: [{$sort: {time}}, groupStage],
                    expectStageWithIndex:
                        (canSortOnTimeUseDistinct ? expectDistinctScan : expectCollScan),
                },

                // Test pipeline without a preceding $match stage with a sort on the index.
                {
                    pipeline: [{$sort: index}, groupStage],
                    expectStageWithIndex: (canUseDistinct ? expectDistinctScan : expectCollScan),
                },

                // Test pipeline with a projection to ensure that we correctly evaluate
                // computedMetaProjFields in the rewrite. Note that we can't get a DISTINCT_SCAN
                // here due to the projection.
                {
                    pipeline: [
                        {$set: {abc: {$add: [1, "$tags.hostid"]}}},
                        {$sort: index},
                        getGroupStage(time, ["abc"]),
                    ],
                    expectStageWithIndex: expectCollscanNoSort,
                    expectStageNoIndex: expectCollscanNoSort,
                },

                // Test pipeline with an equality $match stage.
                getTestWithMatch({$match: {"tags.hostid": 0}}, {"meta.hostid": {$eq: 0}}),

                // Test pipeline with an inequality $match stage.
                getTestWithMatch({$match: {"tags.hostid": {$ne: 0}}},
                                 {"meta.hostid": {$not: {$eq: 0}}}),

                // Test pipeline with a $match stage that uses a $gt query.
                getTestWithMatch({$match: {"tags.hostid": {$gt: 5}}}, {"meta.hostid": {$gt: 5}}),

                // Test pipeline with a $match stage that uses a $lt query.
                getTestWithMatch({$match: {"tags.hostid": {$lt: 5}}}, {"meta.hostid": {$lt: 5}}),
            ];
        });
}

// Test pipeline without a preceding $match stage which has an extra idle measurement. This verifies
// that the query rewrite correctly returns missing fields.
{
    const [tsColl, observerColl] = createBoringCollections(true /* includeIdleMeasurements */);
    testAllTimeMetaDirections(
        tsColl,
        observerColl,
        ({canUseDistinct, index, time}) => [{
            pipeline: [{$sort: index}, getGroupStage(time)],
            expectStageWithIndex: (canUseDistinct ? expectDistinctScan : expectCollScan),
        }]);
}

// Test interesting metaField values.
{
    const [tsColl, observerColl] = createInterestingCollections();
    const expectIxscanNoSort = ({explain}) => expectIxscan({explain, noSortInCursor: true});

    // Verifies that the '_id' of each group matches one of the equivalent '_id' values.
    const mapToEquivalentIdStage = getMapInterestingValuesToEquivalentsStage();

    testAllTimeMetaDirections(
        tsColl, observerColl, ({canUseDistinct, canSortOnTimeUseDistinct, time, index}) => {
            const groupStage = getGroupStage(time);
            return [
                // Test pipeline with sort only on time and interesting metaField values.
                {
                    pipeline: [{$sort: {time}}, groupStage, mapToEquivalentIdStage],
                    // We get an index scan here because the index on interesting values is
                    // multikey.
                    expectStageWithIndex:
                        (canSortOnTimeUseDistinct ? expectIxscanNoSort : expectCollScan),
                },
                // Test pipeline without a preceding $match stage and interesting metaField values.
                {
                    pipeline: [{$sort: index}, groupStage, mapToEquivalentIdStage],
                    // We get an index scan here because the index on interesting values is
                    // multikey, so we cannot have a DISTINCT_SCAN.
                    expectStageWithIndex: (canUseDistinct ? expectIxscanNoSort : expectCollScan),
                }
            ];
        });
}
})();
