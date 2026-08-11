/**
 * Tests the serverStatus metrics for the persistent-samples write path (analyze command in
 * "sample" mode) and read path (SamplingEstimatorImpl loading a persisted sample).
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getWinningPlanFromExplain, isCollscan} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerConfig, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
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
    // query.analyze.sample.histograms.{pages} and query.sampling.persistentSample.histograms.
    // {pagesRead} use pow(6, 1, 2)
    const kPagesHistogramBounds = histogramPowBounds(6, 1, 2);
    // query.analyze.sample.histograms.bytesPersisted uses pow(11, 256, 4).
    const kBytesHistogramBounds = histogramPowBounds(11, 256, 4);

    before(function () {
        conn = MongoRunner.runMongod({setParameter: {featureFlagPersistentStats: true}});
        assert.neq(conn, null, "mongod failed to start");
        db = conn.getDB(jsTestName());
        coll = db[collName];

        // In the legacy (non-deferred) getExecutor path, CBR is disabled for queries that will be
        // executed via SBE. When we are running in the legacy getExecutor path and SBE is fully enabled,
        // then we will always build an SBE plan for the query so CBR will never run as expected in the
        // test.
        // TODO SERVER-119581: Once the feature flag controlling the deferred engine selection is
        // deleted, this block should be able to be deleted.
        if (
            checkSbeFullyEnabled(db) &&
            !FeatureFlagUtil.isEnabled(db, "GetExecutorDeferredEngineChoice")
        ) {
            jsTest.log.info(
                `Skipping ${jsTestName()}: CBR is not run for SBE plans without deferred engine choice`,
            );
            MongoRunner.stopMongod(conn);
            quit();
        }

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

    it("excludes discarded documents from docsPersisted", function () {
        const discardCollName = collName + "_discard";
        const discardColl = db[discardCollName];
        discardColl.drop();

        // 1 of 20 docs (5%) is unpersistable, which is within the 10% discard budget.
        const bigDoc = PersistentSamplesUtils.makeDocOfSize(16 * 1024 * 1024); // 16 MB
        const smallDoc = PersistentSamplesUtils.makeDocOfSize(1024);
        const kNumDocs = 20;
        const docs = Array.from({length: kNumDocs}, (_, i) =>
            Object.assign({}, i === 10 ? bigDoc : smallDoc, {_id: i}),
        );
        docs.forEach((doc) => assert.commandWorked(discardColl.insert(doc)));

        const before = getAnalyzeMetrics(db);

        // sampleSize == numRecords so all docs will be in the sample.
        assert.commandWorked(
            db.runCommand({
                analyze: discardCollName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kNumDocs,
            }),
        );

        const after = getAnalyzeMetrics(db);

        PersistentSamplesUtils.validatePersistentSample(db, {
            sampledCollName: discardCollName,
            samplingMethod: "random",
            requestedSampleSize: kNumDocs,
            // The oversized doc is discarded, so only 19 docs are persisted.
            actualSampleSize: kNumDocs - 1,
            expectedFields: ["pad"],
            expectedNumPages: 1,
        });

        assert.eq(
            after.docsPersisted,
            before.docsPersisted + kNumDocs - 1,
            "docsPersisted should not count the doc discarded during paging",
            {before, after},
        );

        discardColl.drop();
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

    // Sums the exact BSON size of every persisted page.
    function sumPageBytes(pages) {
        return pages.reduce((total, page) => total + Object.bsonsize(page), 0);
    }

    it("records a single-page sample in the pages and bytes metrics", function () {
        const before = getAnalyzeMetrics(db);

        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kSampleSize,
            }),
        );

        const after = getAnalyzeMetrics(db);

        const pages = PersistentSamplesUtils.validatePersistentSample(db, {
            sampledCollName: collName,
            samplingMethod: "random",
            requestedSampleSize: kSampleSize,
            actualSampleSize: kSampleSize,
            expectedFields: ["a"],
            expectedNumPages: 1,
        });

        assert.eq(
            sumHistogramBucketCounts(after.histograms.pages),
            sumHistogramBucketCounts(before.histograms.pages) + 1,
            "pages histogram should gain one observation",
            {before, after},
        );
        assertHistogramBoundaries(after.histograms.pages, kPagesHistogramBounds);

        const totalBytes = sumPageBytes(pages);
        assert.eq(
            after.totalBytesPersisted,
            before.totalBytesPersisted + totalBytes,
            "totalBytesPersisted should increase by the summed size of the persisted pages",
            {before, after, totalBytes},
        );
        assert.eq(
            sumHistogramBucketCounts(after.histograms.bytesPersisted),
            sumHistogramBucketCounts(before.histograms.bytesPersisted) + 1,
            "bytesPersisted histogram should gain one observation",
            {before, after},
        );
        assertHistogramBoundaries(after.histograms.bytesPersisted, kBytesHistogramBounds);
    });

    it("records pagesRead for a single-page read-path load", function () {
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
        assert(isCollscan(db, getWinningPlanFromExplain(explain)), "expected a COLLSCAN plan", {
            explain,
        });

        const after = getPersistentSampleMetrics(db);

        assert.eq(after.hits, before.hits + 1, "the load should be a persisted-sample hit", {
            before,
            after,
        });
        assert.eq(
            sumHistogramBucketCounts(after.histograms.pagesRead),
            sumHistogramBucketCounts(before.histograms.pagesRead) + 1,
            "pagesRead histogram should gain one observation",
            {before, after},
        );
        assertHistogramBoundaries(after.histograms.pagesRead, kPagesHistogramBounds);
    });

    it("records sampleAgeAtReadMillis for a persisted read-path load", function () {
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
        assert(isCollscan(db, getWinningPlanFromExplain(explain)), "expected a COLLSCAN plan", {
            explain,
        });

        const after = getPersistentSampleMetrics(db);

        assert.eq(after.hits, before.hits + 1, "the load should be a persisted-sample hit", {
            before,
            after,
        });
        assert.eq(
            sumHistogramBucketCounts(after.histograms.sampleAgeAtReadMillis),
            sumHistogramBucketCounts(before.histograms.sampleAgeAtReadMillis) + 1,
            "sampleAgeAtReadMillis histogram should gain one observation",
            {before, after},
        );
    });

    describe("multi-page samples", function () {
        const largeCollName = collName + "_large";
        let largeColl;

        before(function () {
            largeColl = db[largeCollName];
            // Persisted sample >16 mb is forced to span more than one page.
            const docBytes = 50 * 1024;
            const docs = PersistentSamplesUtils.makeDocsOfTotalSize(
                kSampleSize,
                docBytes * kSampleSize,
            );
            assert.commandWorked(largeColl.insertMany(docs));
        });

        // Persists a sample from the large collection and returns its page documents ordered by
        // pageNo.
        function persistMultiPageSample() {
            assert.commandWorked(
                db.runCommand({
                    analyze: largeCollName,
                    mode: "sample",
                    samplingMethod: "random",
                    sampleSize: kSampleSize,
                }),
            );
            const pages = PersistentSamplesUtils.getSamplesColl(db)
                .find()
                .sort({"_id.pageNo": 1})
                .toArray();
            assert.gt(pages.length, 1, "setup should persist a multi-page sample", {
                numPages: pages.length,
            });
            return pages;
        }

        it("records a multi-page sample in the pages and bytes metrics", function () {
            const before = getAnalyzeMetrics(db);
            const pages = persistMultiPageSample();
            const after = getAnalyzeMetrics(db);

            assert.eq(
                sumHistogramBucketCounts(after.histograms.pages),
                sumHistogramBucketCounts(before.histograms.pages) + 1,
                "pages histogram should gain one observation",
                {before, after},
            );
            assertHistogramBoundaries(after.histograms.pages, kPagesHistogramBounds);

            const totalBytes = sumPageBytes(pages);
            assert.eq(
                after.totalBytesPersisted,
                before.totalBytesPersisted + totalBytes,
                "totalBytesPersisted should increase by the summed size of every persisted page",
                {before, after, totalBytes},
            );
            assert.eq(
                sumHistogramBucketCounts(after.histograms.bytesPersisted),
                sumHistogramBucketCounts(before.histograms.bytesPersisted) + 1,
                "bytesPersisted histogram should gain one observation",
                {before, after},
            );
            assertHistogramBoundaries(after.histograms.bytesPersisted, kBytesHistogramBounds);
        });

        it("records pagesRead for a multi-page read-path load", function () {
            const pages = persistMultiPageSample();
            const before = getPersistentSampleMetrics(db);
            const explain = largeColl.find({pad: {$gte: ""}}).explain();
            assert(isCollscan(db, getWinningPlanFromExplain(explain)), "expected a COLLSCAN plan", {
                explain,
            });

            const after = getPersistentSampleMetrics(db);

            assert.eq(after.hits, before.hits + 1, "the load should be a persisted-sample hit", {
                before,
                after,
            });
            assert.eq(
                sumHistogramBucketCounts(after.histograms.pagesRead),
                sumHistogramBucketCounts(before.histograms.pagesRead) + 1,
                "pagesRead histogram should gain one observation",
                {before, after},
            );
            assertHistogramBoundaries(after.histograms.pagesRead, kPagesHistogramBounds);
        });
    });
});
