/**
 * Tests basic raw index creation and operations directly on buckets, by using rawData operations.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {
    createRawTimeseriesIndex,
    getTimeseriesCollForRawOps,
    kRawOperationSpec,
} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getWinningPlanFromExplain, isIdhackOrExpress, planHasStage} from "jstests/libs/query/analyze_plan.js";

TimeseriesTest.run((insert) => {
    const coll = db[jsTestName()];

    const timeFieldName = "time";

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.commandWorked(createRawTimeseriesIndex(coll, {"control.min.time": 1}));

    const t = new Date();
    const doc = {_id: 0, [timeFieldName]: t, x: 0};
    assert.commandWorked(insert(coll, doc), "failed to insert doc: " + tojson(doc));

    assert.commandWorked(createRawTimeseriesIndex(coll, {"control.max.time": 1}));

    let buckets = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
    assert.eq(buckets.length, 1, "Expected one bucket but found " + tojson(buckets));
    const bucketId = buckets[0]._id;
    const minTime = buckets[0].control.min.time;
    const maxTime = buckets[0].control.max.time;

    assert.docEq(buckets, getTimeseriesCollForRawOps(coll).find({_id: bucketId}).rawData().toArray());
    let explain = getTimeseriesCollForRawOps(coll).find({_id: bucketId}).rawData().explain();
    assert(isIdhackOrExpress(db, getWinningPlanFromExplain(explain)), explain);

    assert.docEq(buckets, getTimeseriesCollForRawOps(coll).find({"control.max.time": maxTime}).rawData().toArray());
    explain = getTimeseriesCollForRawOps(coll).find({"control.max.time": minTime}).rawData().explain();
    assert(planHasStage(db, getWinningPlanFromExplain(explain), "IXSCAN"), explain);

    let res = assert.commandWorked(coll.validate());
    assert(res.valid, res);

    assert.commandWorked(getTimeseriesCollForRawOps(coll).remove({_id: bucketId}, kRawOperationSpec));
    assert.docEq([], getTimeseriesCollForRawOps(coll).find().rawData().toArray());
});
