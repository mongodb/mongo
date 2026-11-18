/**
 * Tests that bucket min/max time metadata the shards produce when unpacking timeseries buckets is
 * available to the merging half of a sharded aggregation, both to a $_internalBoundedSort and when
 * read directly with $meta.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */
import {
    metaFieldName,
    prepareShardedCollection,
    setUpShardedCluster,
    tearDownShardedCluster,
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("bucket min/max time metadata over a sharded timeseries collection", function () {
    let coll;
    // Five buckets of two measurements, two hours apart, so bucket min and max time differ.
    const startTime = ISODate("2026-01-01T00:00:00Z").getTime();
    const bucketSpanMs = 2 * 60 * 60 * 1000;
    const bucketMinTime = (id) => new Date(startTime + Math.floor(id / 2) * bucketSpanMs);
    const bucketMaxTime = (id) => new Date(bucketMinTime(id).getTime() + 1000);
    const docs = Array.from({length: 10}, (_, i) => ({
        _id: i,
        time: new Date(bucketMinTime(i).getTime() + (i % 2) * 1000),
        [metaFieldName]: "A",
        value: i,
    }));

    before(function () {
        setUpShardedCluster();
        // Split across both shards so the aggregation runs partly on the merger.
        coll = prepareShardedCollection({collName: jsTestName(), initialDocList: docs});
    });

    after(function () {
        tearDownShardedCluster();
    });

    const cases = [
        {base: "min", metaType: "timeseriesBucketMinTime", expectedBucketTime: bucketMinTime},
        {base: "max", metaType: "timeseriesBucketMaxTime", expectedBucketTime: bucketMaxTime},
    ];
    for (const {base, metaType, expectedBucketTime} of cases) {
        const pipeline = [
            {$skip: 1},
            {$_internalBoundedSort: {sortKey: {time: 1}, bound: {base}}},
            {$addFields: {bucketTime: {$meta: metaType}}},
        ];
        it(`reads the bucket ${base} time metadata in a bounded sort`, function () {
            // The metadata only crosses the shard->mongos boundary if the pipeline is split, so
            // fail rather than silently pass if the shards are no longer each targeted.
            const explain = coll.explain().aggregate(pipeline);
            assert(explain.splitPipeline, "Expected the pipeline to be split", {explain});
            assert(
                explain.splitPipeline.mergerPart.some((stage) => "$_internalBoundedSort" in stage),
                "Expected the bounded sort on the merging half",
                {mergerPart: explain.splitPipeline.mergerPart},
            );

            const result = coll.aggregate(pipeline).toArray();
            assert.eq(docs.length - 1, result.length);

            for (const doc of result) {
                assert.eq(expectedBucketTime(doc._id), doc.bucketTime, "Wrong bucket time", {doc});
            }
        });
    }
});
