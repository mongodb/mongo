/**
 * Tests that the duration of an index build, and the number of keys and bytes it wrote to the
 * index tables, are recorded in the completion metrics by the start phase and outcome of the
 * build.
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
    assertHistogramCountAcrossFilesSoon,
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

// The bucket boundaries of the keys and bytes written histograms.
const kKeysWrittenBounds = [0, 10, 100, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10];
const kBytesWrittenBounds = [
    0, 1024, 8192, 65536, 524288, 4194304, 33554432, 268435456, 2147483648, 17179869184,
    137438953472, 1099511627776,
];

// The size of the key string generated from a sample document {x: 1}. Every key generated from documents in this test should have this size.
const kKeyStringBytesPerKey = 5;

/**
 * Waits for every completion metric to have recorded `expected` observations for this attribute
 * value, across every metrics file in `metricsDir`. The first describe gives each node its own
 * directory and passes only the primary's, so there this reads a single node. The resumed test
 * shares one directory (`_pdibMetricsDir`) between the nodes, because the resumed build completes
 * on whichever node steps up, so its observation lands in a different file than the interrupted
 * build's.
 */
function assertAllCompletionMetricsCountSoon(metricsDir, attrKey, attrValue, expected) {
    for (const metricName of kCompletionMetrics) {
        assertHistogramCountAcrossFilesSoon({metricsDir, metricName, attrKey, attrValue, expected});
    }
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
        for (const metricName of kCompletionMetrics) {
            assert.eq(
                getHistogramByAttributeAcrossFiles(
                    this.metricsDir,
                    metricName,
                    "outcome",
                    "success",
                ).count,
                0,
            );
        }

        assert.commandWorked(this.coll.createIndex({a: 1}));

        assertAllCompletionMetricsCountSoon(this.metricsDir, "outcome", "success", 1);

        // One key per document is bulk loaded into the index table. Nothing writes to the
        // collection while the build runs, so the side writes drain contributes nothing.
        const keys = getHistogramByAttributeAcrossFiles(
            this.metricsDir,
            kKeysWrittenMetric,
            "outcome",
            "success",
        );
        assert.eq(keys.sum, 3, "keys written by the successful build", {keys});
        assertHistogramSingleObservation(keys, kKeysWrittenBounds, 3);

        const bytes = getHistogramByAttributeAcrossFiles(
            this.metricsDir,
            kBytesWrittenMetric,
            "outcome",
            "success",
        );
        assert.eq(bytes.sum, 3 * kKeyStringBytesPerKey, "bytes written by the successful build", {
            bytes,
        });
        assertHistogramSingleObservation(bytes, kBytesWrittenBounds, 3 * kKeyStringBytesPerKey);
    });

    it("failed index build", () => {
        // Fail a unique index build by inserting duplicate keys.
        assert.commandWorked(this.coll.insert([{a: 1}, {a: 1}]));
        for (const metricName of kCompletionMetrics) {
            assert.eq(
                getHistogramByAttributeAcrossFiles(
                    this.metricsDir,
                    metricName,
                    "outcome",
                    "failure",
                ).count,
                0,
            );
        }

        assert.commandFailedWithCode(
            this.coll.createIndex({a: 1}, {unique: true}),
            ErrorCodes.DuplicateKey,
        );

        assertAllCompletionMetricsCountSoon(this.metricsDir, "outcome", "failure", 1);

        // A build that fails still reports the work it did before failing. The bulk load allows
        // duplicates and records them, and they are only rejected when the build checks its
        // constraints before committing, so both keys have been written by then.
        const keys = getHistogramByAttributeAcrossFiles(
            this.metricsDir,
            kKeysWrittenMetric,
            "outcome",
            "failure",
        );
        assert.eq(keys.sum, 2, "keys written by the failed build", {keys});

        const bytes = getHistogramByAttributeAcrossFiles(
            this.metricsDir,
            kBytesWrittenMetric,
            "outcome",
            "failure",
        );
        assert.eq(bytes.sum, 2 * kKeyStringBytesPerKey, "bytes written by the failed build", {
            bytes,
        });
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

    it("attributes a resumed build to the phase it resumed from", () => {
        const db = this.rst.getPrimary().getDB(dbName);
        if (
            !FeatureFlagUtil.isPresentAndEnabled(db, "ContainerWrites") ||
            !FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds") ||
            !FeatureFlagUtil.isPresentAndEnabled(db, "ResumablePrimaryDrivenIndexBuilds")
        ) {
            jsTest.log.info("Skipping: resumable primary-driven index builds are disabled");
            return;
        }

        for (const metricName of kCompletionMetrics) {
            assert.eq(
                getHistogramByAttributeAcrossFiles(
                    this.rst._pdibMetricsDir,
                    metricName,
                    "start_phase",
                    "collection scan",
                ).count,
                0,
            );
        }

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

        assertAllCompletionMetricsCountSoon(
            this.rst._pdibMetricsDir,
            "start_phase",
            "collection scan",
            1,
        );
        assertAllCompletionMetricsCountSoon(this.rst._pdibMetricsDir, "outcome", "success", 1);

        // The counters live on the MultiIndexBlock, which is rebuilt when the build resumes, so
        // these report only what was written after resuming. The build was interrupted before it
        // bulk loaded anything, so the resumed build loads every key of all three indexes.
        const keys = getHistogramByAttributeAcrossFiles(
            this.rst._pdibMetricsDir,
            kKeysWrittenMetric,
            "start_phase",
            "collection scan",
        );
        assert.eq(keys.sum, 3 * docCount, "keys written by the resumed build", {keys});

        const bytes = getHistogramByAttributeAcrossFiles(
            this.rst._pdibMetricsDir,
            kBytesWrittenMetric,
            "start_phase",
            "collection scan",
        );
        assert.eq(
            bytes.sum,
            3 * docCount * kKeyStringBytesPerKey,
            "bytes written by the resumed build",
            {bytes},
        );
    });
});
