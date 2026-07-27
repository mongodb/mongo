/**
 * Tests the serverStatus metrics for the persistent-samples write path (analyze command in
 * "sample" mode) and read path (SamplingEstimatorImpl loading a persisted sample).
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getWinningPlanFromExplain, isCollscan} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerConfig, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";
import {
    assertHistogramBoundaries,
    getAnalyzeCommandMetrics,
    getAnalyzeMetrics,
    getPersistentSampleMetrics,
    histogramPowBounds,
    sumHistogramBucketCounts,
} from "jstests/libs/query/planning_metrics_utils.js";

describe("persistent sample serverStatus metrics", function () {
    let conn;
    let db;
    let coll;

    const collName = jsTestName();
    const kSourceSize = 1000;
    const kSourceDocs = Array.from({length: kSourceSize}, (_, i) => ({_id: i, a: i}));
    let kSampleSize;

    // Both commands.analyze.histograms.micros and query.sampling.persistentSample.histograms.
    // loadMicros use HistogramServerStatusMetric::pow(11, 256, 4).
    const kMicrosHistogramBounds = histogramPowBounds(11, 256, 4);

    before(function () {
        conn = MongoRunner.runMongod({setParameter: {featureFlagPersistentStats: true}});
        assert.neq(conn, null, "mongod failed to start");
        db = conn.getDB(jsTestName());
        coll = db[collName];

        setPlanRankerConfig(db, {
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
        });
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryDisablePlanCache: 1,
                internalQuerySamplingBySequentialScan: false,
                internalQuerySamplingByStrides: false,
                internalQuerySamplingCEMethod: "random",
                internalQuerySamplingCEMethodForPersistentSamples: "random",
            }),
        );

        kSampleSize = PersistentSamplesUtils.defaultSampleSize(db);
        assert.lt(
            kSampleSize,
            kSourceSize,
            "default sample size must be smaller than the source collection",
        );

        assert.commandWorked(coll.insertMany(kSourceDocs));
    });

    beforeEach(function () {
        PersistentSamplesUtils.dropSamplesColl(db);
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("increments write-path metrics for a random-technique run", function () {
        const before = getAnalyzeMetrics(db);
        const beforeCmd = getAnalyzeCommandMetrics(db);

        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kSampleSize,
            }),
        );

        const after = getAnalyzeMetrics(db);
        const afterCmd = getAnalyzeCommandMetrics(db);

        assert.eq(
            after.docsPersisted,
            before.docsPersisted + kSampleSize,
            "docsPersisted should increase by the actual persisted doc count",
            {before, after},
        );
        assert.eq(
            after.byMethod.random,
            before.byMethod.random + 1,
            "byMethod.random should increment",
            {before, after},
        );
        assert.eq(after.byMethod.chunk, before.byMethod.chunk, "byMethod.chunk should not change", {
            before,
            after,
        });
        assert.eq(
            after.byMethod.fullCollScan,
            before.byMethod.fullCollScan,
            "byMethod.fullCollScan should not change",
            {before, after},
        );
        assert.eq(afterCmd.total, beforeCmd.total + 1, "commands.analyze.total should increment", {
            beforeCmd,
            afterCmd,
        });
        assert.eq(
            sumHistogramBucketCounts(afterCmd.histograms.micros),
            sumHistogramBucketCounts(beforeCmd.histograms.micros) + 1,
            "commands.analyze.histograms.micros bucket count should increase by 1",
            {beforeCmd, afterCmd},
        );
        assertHistogramBoundaries(afterCmd.histograms.micros, kMicrosHistogramBounds);
        assert.eq(
            afterCmd.rejected,
            beforeCmd.rejected,
            "commands.analyze.rejected should not change on a successful run",
            {beforeCmd, afterCmd},
        );

        // Run a second time to confirm metrics accumulate rather than reset.
        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kSampleSize,
            }),
        );
        const afterSecond = getAnalyzeMetrics(db);
        assert.eq(
            afterSecond.docsPersisted,
            after.docsPersisted + kSampleSize,
            "docsPersisted should accumulate across runs",
            {after, afterSecond},
        );
        assert.eq(
            afterSecond.byMethod.random,
            after.byMethod.random + 1,
            "byMethod.random should accumulate",
            {after, afterSecond},
        );
        assert.eq(
            afterSecond.byMethod.chunk,
            after.byMethod.chunk,
            "byMethod.chunk should not change",
            {after, afterSecond},
        );
        assert.eq(
            afterSecond.byMethod.fullCollScan,
            after.byMethod.fullCollScan,
            "byMethod.fullCollScan should not change",
            {after, afterSecond},
        );
    });

    it("increments write-path metrics for a chunk-technique run", function () {
        const before = getAnalyzeMetrics(db);

        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "chunk",
                sampleSize: kSampleSize,
            }),
        );

        const after = getAnalyzeMetrics(db);

        // Chunk sampling persists a variable number of docs, so only assert the counter increased.
        assert.gt(
            after.docsPersisted,
            before.docsPersisted,
            "docsPersisted should increase for a chunk-technique run",
            {before, after},
        );
        assert.eq(
            after.byMethod.chunk,
            before.byMethod.chunk + 1,
            "byMethod.chunk should increment",
            {before, after},
        );
        assert.eq(
            after.byMethod.random,
            before.byMethod.random,
            "byMethod.random should not change",
            {before, after},
        );
        assert.eq(
            after.byMethod.fullCollScan,
            before.byMethod.fullCollScan,
            "byMethod.fullCollScan should not change",
            {before, after},
        );
    });

    it("increments write-path metrics for a full-collection-scan run", function () {
        const before = getAnalyzeMetrics(db);

        // A sample size >= the collection size forces a full collection scan.
        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kSourceSize,
            }),
        );

        const after = getAnalyzeMetrics(db);

        assert.eq(
            after.docsPersisted,
            before.docsPersisted + kSourceSize,
            "docsPersisted should increase by the whole collection size on a full scan",
            {before, after},
        );
        assert.eq(
            after.byMethod.fullCollScan,
            before.byMethod.fullCollScan + 1,
            "byMethod.fullCollScan should increment",
            {before, after},
        );
        assert.eq(
            after.byMethod.random,
            before.byMethod.random,
            "byMethod.random should not change",
            {before, after},
        );
        assert.eq(after.byMethod.chunk, before.byMethod.chunk, "byMethod.chunk should not change", {
            before,
            after,
        });
    });

    it("increments read-path hit metrics when a persisted sample is loaded", function () {
        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kSampleSize,
            }),
        );

        const before = getPersistentSampleMetrics(db);

        const explain = coll.find({a: {$gte: 0}}).explain();
        const plan = getWinningPlanFromExplain(explain);
        assert(isCollscan(db, plan), "expected a COLLSCAN plan", {plan});

        const after = getPersistentSampleMetrics(db);

        assert.eq(after.hits, before.hits + 1, "hits should increment on a persisted-sample load", {
            before,
            after,
        });
        assert.eq(
            after.docsLoaded,
            before.docsLoaded + kSampleSize,
            "docsLoaded should increase by the loaded sample size",
            {before, after},
        );
        assert.eq(
            sumHistogramBucketCounts(after.histograms.loadMicros),
            sumHistogramBucketCounts(before.histograms.loadMicros) + 1,
            "histograms.loadMicros bucket count should increase by 1",
            {before, after},
        );
        assertHistogramBoundaries(after.histograms.loadMicros, kMicrosHistogramBounds);
        assert.eq(after.misses, before.misses, "misses should not change on a hit", {
            before,
            after,
        });
    });

    it("increments read-path miss metrics when no persisted sample exists", function () {
        // No analyze run — nothing persisted for this collection.

        const before = getPersistentSampleMetrics(db);

        const explain = coll.find({a: {$gte: 0}}).explain();
        const plan = getWinningPlanFromExplain(explain);
        assert(isCollscan(db, plan), "expected a COLLSCAN plan", {plan});

        const after = getPersistentSampleMetrics(db);

        assert.eq(after.misses, before.misses + 1, "misses should increment on a miss", {
            before,
            after,
        });
        assert.eq(after.hits, before.hits, "hits should not change on a miss", {before, after});
    });

    it("automatically tracks commands.analyze.{total,failed,rejected} for every command", function () {
        const before = getAnalyzeCommandMetrics(db);

        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kSampleSize,
            }),
        );
        const afterSuccess = getAnalyzeCommandMetrics(db);
        assert.eq(
            afterSuccess.total,
            before.total + 1,
            "commands.analyze.total should increment on success",
            {before, afterSuccess},
        );
        assert.eq(
            afterSuccess.failed,
            before.failed,
            "commands.analyze.failed should not change on success",
            {before, afterSuccess},
        );
        assert.eq(
            afterSuccess.rejected,
            before.rejected,
            "commands.analyze.rejected should not change on success",
            {before, afterSuccess},
        );

        assert.commandFailed(
            db.runCommand({
                analyze: "nonexistentCollection",
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kSampleSize,
            }),
        );
        const afterFailure = getAnalyzeCommandMetrics(db);
        assert.eq(
            afterFailure.total,
            afterSuccess.total + 1,
            "commands.analyze.total should also increment on failure",
            {afterSuccess, afterFailure},
        );
        assert.eq(
            afterFailure.failed,
            afterSuccess.failed + 1,
            "commands.analyze.failed should increment on failure",
            {afterSuccess, afterFailure},
        );
        assert.eq(
            afterFailure.rejected,
            before.rejected,
            "commands.analyze.rejected should be unaffected (analyze can't be rejected by query settings)",
            {before, afterFailure},
        );
    });
});
