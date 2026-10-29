/**
 * For an untracked viewless timeseries collection, mongos must defer timeseries rewriting and
 * pipeline optimization until after dispatch. Verify via the profiler that separate $match stages
 * remain unmerged and no $_internalUnpackBucket stage is added, and verify the results are correct.
 *
 * @tags: [
 *   requires_profiling,
 *   requires_sharding,
 *   requires_timeseries,
 *   # Viewless timeseries collections (and the untrackUnshardedCollection /
 *   # createUnsplittableCollection commands this test relies on) require FCV 9.0.
 *   requires_fcv_90,
 * ]
 */
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const timeField = "time";
const metaField = "meta";

function stageName(stage) {
    return Object.keys(stage)[0];
}

// $bucket forces requiresCollationForParsingUnshardedAggregate() to be true (see
// document_source_bucket.h), which is what allows mongos to attempt optimization even without a
// routing table. The two leading $match stages are only ever merged into one by generic pipeline
// optimization.
const bucketPipeline = [
    {$match: {a: {$gte: 0}}},
    {$match: {a: {$lt: 100}}},
    {$bucket: {groupBy: "$bucketValue", boundaries: ["0", "9", "z"], default: "zzz"}},
];
const rawDataBucketPipeline = [
    {$match: {"control.version": {$gte: 1}}},
    {$match: {"control.version": {$lt: 10}}},
    {$bucket: {groupBy: "$control.version", boundaries: [1, 10], default: "other"}},
];
const matchOnlyPipeline = [bucketPipeline[0], bucketPipeline[1]];

// The tests below insert ten measurements into each of the two buckets.
function makeTestDocuments() {
    return Array.from({length: 10}, (_, i) => [
        {[timeField]: ISODate(), [metaField]: 0, a: 10 + i, bucketValue: String(10 + i)},
        {[timeField]: ISODate(), [metaField]: 0, a: 60 + i, bucketValue: String(60 + i)},
    ]).flat();
}

// Expected results of running 'bucketPipeline' / 'matchOnlyPipeline' against the generated
// documents.
// Under the simple collation, all values sort before "9". With numericOrdering, all values sort
// between "9" and "z".
const expectedSimpleBucketResults = [{_id: "0", count: 20}];
const expectedNumericBucketResults = [{_id: "9", count: 20}];
const expectedRawDataBucketResults = [{_id: 1, count: 1}];
const expectedMatchOnlyAValues = makeTestDocuments().map((doc) => doc.a);
const numericOrderingCollation = {locale: "en_US", numericOrdering: true};

describe("$bucket on an untracked viewless timeseries collection", function () {
    let st, testDB, shardDB;

    before(function () {
        st = new ShardingTest({shards: 1});
        testDB = st.s.getDB(dbName);
        assert.commandWorked(
            testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        shardDB = st.shard0.getDB(dbName);

        if (!areViewlessTimeseriesEnabled(testDB)) {
            jsTest.log.info("Skipping: this bug only applies to viewless timeseries collections");
            st.stop();
            quit();
        }
    });

    after(function () {
        st.stop();
    });

    // Runs 'pipeline' (with the given aggregate 'options') against 'coll' and returns both the
    // exact pipeline mongos dispatched to the shard (as captured by the shard's profiler) and the
    // aggregation's results.
    function runAndGetDispatchedPipeline(coll, pipeline, options = {}) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);

        const results = coll.aggregate(pipeline, options).toArray();

        const entries = shardDB.system.profile
            .find({"command.aggregate": coll.getName()})
            .sort({ts: -1})
            .toArray();
        assert.eq(entries.length, 1, "expected exactly one profiler entry for the aggregate", {
            collName: coll.getName(),
            entries,
        });

        shardDB.setProfilingLevel(0);
        return {dispatched: entries[0].command.pipeline, results};
    }

    it("defers both the rewrite and the optimization for $bucket on an untracked collection", function () {
        const coll = testDB.untracked_ts;
        coll.drop();
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {timeseries: {timeField, metaField}}),
        );
        assert.commandWorked(testDB.adminCommand({untrackUnshardedCollection: coll.getFullName()}));
        assert.commandWorked(coll.insertMany(makeTestDocuments()));

        const {dispatched, results} = runAndGetDispatchedPipeline(coll, bucketPipeline);

        // The timeseries rewrite cannot run on mongos without a routing table, so no
        // $_internalUnpackBucket stage should have been prepended before dispatch.
        assert(
            !dispatched.some((s) => stageName(s) === "$_internalUnpackBucket"),
            "expected no $_internalUnpackBucket stage to have been dispatched",
            {dispatched},
        );

        // Correct behavior: since the rewrite hasn't happened, mongos must also defer
        // optimization to the shard. The two originally-separate $match stages should therefore
        // still be dispatched unmerged.
        const matchStages = dispatched.filter((s) => stageName(s) === "$match");
        assert.eq(matchStages.length, 2, "expected the two $match stages to remain unmerged", {
            dispatched,
        });

        assert.sameMembers(
            results,
            expectedSimpleBucketResults,
            "unexpected $bucket results",
            undefined,
            {
                results,
            },
        );
    });

    it(
        "defers both the rewrite and the optimization for $bucket on an untracked collection " +
            "with an explicit collation",
        function () {
            // Regression test: an explicit collation used to make mongos skip fetching whether the
            // collection is a viewless timeseries collection, since it could return the
            // user-supplied collation without contacting the primary shard. But that primary-shard
            // contact is also how mongos learns to defer the timeseries rewrite, so supplying a
            // collation must not cause premature optimization either.
            const coll = testDB.untracked_ts_collation;
            coll.drop();
            assert.commandWorked(
                testDB.createCollection(coll.getName(), {timeseries: {timeField, metaField}}),
            );
            assert.commandWorked(
                testDB.adminCommand({untrackUnshardedCollection: coll.getFullName()}),
            );
            assert.commandWorked(coll.insertMany(makeTestDocuments()));

            const {dispatched, results} = runAndGetDispatchedPipeline(coll, bucketPipeline, {
                collation: numericOrderingCollation,
            });

            assert(
                !dispatched.some((s) => stageName(s) === "$_internalUnpackBucket"),
                "expected no $_internalUnpackBucket stage to have been dispatched",
                {dispatched},
            );

            const matchStages = dispatched.filter((s) => stageName(s) === "$match");
            assert.eq(matchStages.length, 2, "expected the two $match stages to remain unmerged", {
                dispatched,
            });

            assert.sameMembers(
                results,
                expectedNumericBucketResults,
                "unexpected $bucket results",
                undefined,
                {
                    results,
                },
            );
        },
    );

    it(
        "defers both the rewrite and the optimization for $bucket on an untracked collection " +
            "with a default collation",
        function () {
            const coll = testDB.untracked_ts_default_collation;
            coll.drop();
            assert.commandWorked(
                testDB.createCollection(coll.getName(), {
                    timeseries: {timeField, metaField},
                    collation: numericOrderingCollation,
                }),
            );
            assert.commandWorked(
                testDB.adminCommand({untrackUnshardedCollection: coll.getFullName()}),
            );
            assert.commandWorked(coll.insertMany(makeTestDocuments()));

            const {dispatched, results} = runAndGetDispatchedPipeline(coll, bucketPipeline);

            assert(
                !dispatched.some((s) => stageName(s) === "$_internalUnpackBucket"),
                "expected no $_internalUnpackBucket stage to have been dispatched",
                {dispatched},
            );

            const matchStages = dispatched.filter((s) => stageName(s) === "$match");
            assert.eq(matchStages.length, 2, "expected the two $match stages to remain unmerged", {
                dispatched,
            });

            assert.sameMembers(
                results,
                expectedNumericBucketResults,
                "unexpected $bucket results",
                undefined,
                {
                    results,
                },
            );
        },
    );

    it(
        "uses the request collation over the collection default collation for $bucket on an " +
            "untracked collection",
        function () {
            const coll = testDB.untracked_ts_collation_override;
            coll.drop();
            assert.commandWorked(
                testDB.createCollection(coll.getName(), {
                    timeseries: {timeField, metaField},
                    collation: numericOrderingCollation,
                }),
            );
            assert.commandWorked(
                testDB.adminCommand({untrackUnshardedCollection: coll.getFullName()}),
            );
            assert.commandWorked(coll.insertMany(makeTestDocuments()));

            const {dispatched, results} = runAndGetDispatchedPipeline(coll, bucketPipeline, {
                collation: {locale: "simple"},
            });

            assert(
                !dispatched.some((s) => stageName(s) === "$_internalUnpackBucket"),
                "expected no $_internalUnpackBucket stage to have been dispatched",
                {dispatched},
            );

            const matchStages = dispatched.filter((s) => stageName(s) === "$match");
            assert.eq(matchStages.length, 2, "expected the two $match stages to remain unmerged", {
                dispatched,
            });

            assert.sameMembers(
                results,
                expectedSimpleBucketResults,
                "unexpected $bucket results",
                undefined,
                {
                    results,
                },
            );
        },
    );

    it("performs the timeseries rewrite before optimizing $bucket on a tracked collection", function () {
        const coll = testDB.tracked_ts;
        coll.drop();
        // Use createUnsplittableCollection (rather than a plain createCollection) to guarantee a
        // genuinely tracked collection: plain createCollection does not necessarily register a
        // config.collections entry for the namespace.
        assert.commandWorked(
            testDB.runCommand({
                createUnsplittableCollection: coll.getName(),
                dataShard: st.shard0.shardName,
                timeseries: {timeField, metaField},
            }),
        );
        assert.commandWorked(coll.insertMany(makeTestDocuments()));

        const {dispatched, results} = runAndGetDispatchedPipeline(coll, bucketPipeline);

        // The collection is tracked, so the rewrite runs before optimization, and
        // $_internalUnpackBucket is present in the dispatched pipeline.
        assert(
            dispatched.some((s) => stageName(s) === "$_internalUnpackBucket"),
            "expected the timeseries rewrite to have run before dispatch",
            {dispatched},
        );

        assert.sameMembers(
            results,
            expectedSimpleBucketResults,
            "unexpected $bucket results",
            undefined,
            {
                results,
            },
        );
    });

    it("optimizes a rawData $bucket on an untracked collection", function () {
        const coll = testDB.untracked_ts_raw_data;
        coll.drop();
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {timeseries: {timeField, metaField}}),
        );
        assert.commandWorked(testDB.adminCommand({untrackUnshardedCollection: coll.getFullName()}));
        assert.commandWorked(coll.insertMany(makeTestDocuments()));

        const {dispatched, results} = runAndGetDispatchedPipeline(coll, rawDataBucketPipeline, {
            rawData: true,
        });

        assert(
            !dispatched.some((s) => stageName(s) === "$_internalUnpackBucket"),
            "expected no $_internalUnpackBucket stage to have been dispatched",
            {dispatched},
        );

        const matchStages = dispatched.filter((s) => stageName(s) === "$match");
        assert.eq(matchStages.length, 1, "expected the two $match stages to be merged", {
            dispatched,
        });

        assert.sameMembers(
            results,
            expectedRawDataBucketResults,
            "unexpected rawData $bucket results",
            undefined,
            {results},
        );
    });

    it("does not prematurely optimize an untracked collection without $bucket", function () {
        const coll = testDB.untracked_no_bucket;
        coll.drop();
        assert.commandWorked(
            testDB.createCollection(coll.getName(), {timeseries: {timeField, metaField}}),
        );
        assert.commandWorked(testDB.adminCommand({untrackUnshardedCollection: coll.getFullName()}));
        assert.commandWorked(coll.insertMany(makeTestDocuments()));

        const {dispatched, results} = runAndGetDispatchedPipeline(coll, matchOnlyPipeline);

        // Without $bucket, requiresCollationForParsingUnshardedAggregate() is false, so mongos
        // correctly defers both the rewrite and the optimization to the shard: the two $match
        // stages are dispatched unmerged.
        const matchStages = dispatched.filter((s) => stageName(s) === "$match");
        assert.eq(matchStages.length, 2, "expected the two $match stages to remain unmerged", {
            dispatched,
        });

        assert.sameMembers(
            results.map((doc) => doc.a),
            expectedMatchOnlyAValues,
            "unexpected match-only results",
            undefined,
            {results},
        );
    });
});
