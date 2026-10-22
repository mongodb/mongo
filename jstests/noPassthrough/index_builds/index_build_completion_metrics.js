/**
 * Tests that the duration of an index build, and the number of keys and bytes it wrote to the
 * index tables, are recorded in the completion metrics, in the expected histogram bucket.
 * @tags: [
 *   requires_otel_build,
 *   requires_replication,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {
    PdibPhase,
    PdibPosition,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";
import {
    assertHistogramSingleObservation,
    getHistogramByAttributeAcrossFiles,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_metrics_file_export_helpers.js";

const dbName = jsTestName();
const collName = "coll";
const kDurationMetric = "mongodb.index_builds.completed.duration_millis";
const kKeysWrittenMetric = "mongodb.index_builds.completed.keys_written";
const kBytesWrittenMetric = "mongodb.index_builds.completed.data_written";

const kCompletionMetrics = [kDurationMetric, kKeysWrittenMetric, kBytesWrittenMetric];

// The bucket boundaries of the duration, keys written and bytes written histograms.
const kDurationBounds = [60000, 3600000, 28800000, 86400000, 259200000];
const kKeysWrittenBounds = [1e6, 5e7, 5e8, 1e9, 5e9, 1e10, 1e11];
const kBytesWrittenBounds = [
    16777216, 1073741824, 8589934592, 17179869184, 68719476736, 137438953472, 1099511627776,
];

// The size of the key string generated from a sample document {x: 1}. Every key generated from documents in this test should have this size.
const kKeyStringBytesPerKey = 5;

/**
 * Reads every completion metric, aggregated across every metrics file in `metricsDir`.
 */
function readCompletionMetrics(metricsDir) {
    const metrics = {};
    for (const metricName of kCompletionMetrics) {
        metrics[metricName] = getHistogramByAttributeAcrossFiles(metricsDir, metricName, null);
    }
    return metrics;
}

/**
 * Returns what was recorded to the histogram `metricName` between the `before` and `after`
 * snapshots of metrics.
 */
function diffHistogram(metricName, before, after) {
    const [b, a] = [before[metricName], after[metricName]];
    return {
        count: a.count - b.count,
        sum: a.sum - b.sum,
        bucketCounts: a.bucketCounts.map((c, i) => c - (b.bucketCounts[i] ?? 0)),
        explicitBounds: a.explicitBounds,
    };
}

/**
 * Waits for every completion metric to have recorded `expected` observations since `before`,
 * across every metrics file in `metricsDir`, and returns the snapshot taken once they have.
 */
function waitForCompletionMetrics(metricsDir, before, expected) {
    let after;
    assert.soon(
        () => {
            after = readCompletionMetrics(metricsDir);
            return kCompletionMetrics.every(
                (metricName) => diffHistogram(metricName, before, after).count == expected,
            );
        },
        () => `expected ${expected} new observations of every completion metric in ${metricsDir}`,
    );
    return after;
}

describe("index build completion metrics", function () {
    before(() => {
        // A secondary records its own observations for the same build, so each node exports to its
        // own directory and the assertions read only the primary's.
        const node0 = otelFileExportParams(`${jsTestName()}_node0`);
        const node1 = otelFileExportParams(`${jsTestName()}_node1`);
        this.rst = new ReplSetTest({
            nodes: [{setParameter: node0.otelParams}, {setParameter: node1.otelParams}],
        });
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.primaryDB = this.primary.getDB(dbName);
        this.secondaryDB = this.rst.getSecondary().getDB(dbName);
        this.metricsDir = [node0.metricsDir, node1.metricsDir][this.rst.getNodeId(this.primary)];
    });

    beforeEach(() => {
        this.coll = this.primaryDB.getCollection(collName);
        this.coll.drop();
    });

    afterEach(() => {
        IndexBuildTest.waitForIndexBuildToStop(this.primaryDB);
        IndexBuildTest.waitForIndexBuildToStop(this.secondaryDB);
    });

    after(() => {
        this.rst.stopSet();
    });

    it("successful index build", () => {
        assert.commandWorked(this.coll.insert([{a: 1}, {a: 2}, {a: 3}]));
        const before = readCompletionMetrics(this.metricsDir);

        assert.commandWorked(this.coll.createIndex({a: 1}));

        const after = waitForCompletionMetrics(this.metricsDir, before, 1);

        // A build in this test finishes well within the first duration bucket.
        assertHistogramSingleObservation(
            diffHistogram(kDurationMetric, before, after),
            kDurationBounds,
            0,
        );

        // One key per document is bulk loaded into the index table. Nothing writes to the
        // collection while the build runs, so the side writes drain contributes nothing.
        const keys = diffHistogram(kKeysWrittenMetric, before, after);
        assert.eq(keys.sum, 3, "keys written by the successful build", {keys});
        assertHistogramSingleObservation(keys, kKeysWrittenBounds, 3);

        const bytes = diffHistogram(kBytesWrittenMetric, before, after);
        assert.eq(bytes.sum, 3 * kKeyStringBytesPerKey, "bytes written by the successful build", {
            bytes,
        });
        assertHistogramSingleObservation(bytes, kBytesWrittenBounds, 3 * kKeyStringBytesPerKey);
    });

    it("failed index build", () => {
        // Fail a unique index build by inserting duplicate keys.
        assert.commandWorked(this.coll.insert([{a: 1}, {a: 1}]));
        const before = readCompletionMetrics(this.metricsDir);

        assert.commandFailedWithCode(
            this.coll.createIndex({a: 1}, {unique: true}),
            ErrorCodes.DuplicateKey,
        );

        const after = waitForCompletionMetrics(this.metricsDir, before, 1);

        // A build in this test finishes well within the first duration bucket.
        assertHistogramSingleObservation(
            diffHistogram(kDurationMetric, before, after),
            kDurationBounds,
            0,
        );

        // A build that fails still reports the work it did before failing. The bulk load allows
        // duplicates and records them, and they are only rejected when the build checks its
        // constraints before committing, so both keys have been written by then.
        const keys = diffHistogram(kKeysWrittenMetric, before, after);
        assert.eq(keys.sum, 2, "keys written by the failed build", {keys});
        assertHistogramSingleObservation(keys, kKeysWrittenBounds, 2);

        const bytes = diffHistogram(kBytesWrittenMetric, before, after);
        assert.eq(bytes.sum, 2 * kKeyStringBytesPerKey, "bytes written by the failed build", {
            bytes,
        });
        assertHistogramSingleObservation(bytes, kBytesWrittenBounds, 2 * kKeyStringBytesPerKey);
    });
});

describe("resumed index build completion metrics", function () {
    before(() => {
        this.rst = PrimaryDrivenResumableIndexBuildTest.setUp({
            testName: jsTestName(),
        });
    });

    after(() => {
        PrimaryDrivenResumableIndexBuildTest.tearDown(this.rst);
    });

    it("records the work done by a resumed build", () => {
        const db = this.rst.getPrimary().getDB(dbName);
        if (
            !FeatureFlagUtil.isPresentAndEnabled(db, "ContainerWrites") ||
            !FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds") ||
            !FeatureFlagUtil.isPresentAndEnabled(db, "ResumablePrimaryDrivenIndexBuilds")
        ) {
            jsTest.log.info("Skipping: resumable primary-driven index builds are disabled");
            return;
        }

        const before = readCompletionMetrics(this.rst._pdibMetricsDir);

        // Resume an index build from the middle. Arbitrary, this was just to choose one resume to
        // test. Every document has the same small integer in each indexed field and there are no
        // side writes, so each index writes exactly one key per document, each of the same size.
        const docCount = 100;
        PrimaryDrivenResumableIndexBuildTest.run(this.rst, {
            phase: PdibPhase.SCAN,
            positions: [PdibPosition.MIDDLE],
            indexSpecs: [{key: {a: 1}}, {key: {b: 1}}, {key: {c: 1}}],
            docTemplate: (i) => ({_id: i, a: 1, b: 1, c: 1}),
            docCount,
            sideWrites: [],
        });

        // When graceful stepdown is not enabled and a failover kills the old primary, only the resumed build is observed; otherwise both attempts are observed.
        const expectedObservations = PrimaryDrivenResumableIndexBuildTest.failoverStopsOldPrimary()
            ? 1
            : 2;
        const after = waitForCompletionMetrics(
            this.rst._pdibMetricsDir,
            before,
            expectedObservations,
        );

        // The counters live on the MultiIndexBlock, which is rebuilt when the build resumes, so
        // these report only what was written after resuming. The build was interrupted before it
        // bulk loaded anything, so the resumed build loads every key of all three indexes.
        const keys = diffHistogram(kKeysWrittenMetric, before, after);
        assert.eq(keys.sum, 3 * docCount, "keys written by the resumed build", {keys});

        const bytes = diffHistogram(kBytesWrittenMetric, before, after);
        assert.eq(
            bytes.sum,
            3 * docCount * kKeyStringBytesPerKey,
            "bytes written by the resumed build",
            {bytes},
        );
    });
});
