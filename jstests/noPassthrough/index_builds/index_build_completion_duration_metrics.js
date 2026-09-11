/**
 * Tests that the duration of an index build is recorded in the durationMillis metric by the start
 * phase and outcome of the build.
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
    findOtelFilesWithSuffix,
    getFlatMetricsList,
    otelFileExportParams,
    readJsonlFile,
} from "jstests/noPassthrough/observability/libs/otel_metrics_file_export_helpers.js";

const dbName = jsTestName();
const collName = "coll";
const kDurationMetric = "mongodb.index_builds.completed.duration_millis";

/**
 * The number of duration observations exported for one attribute value, summed across every node
 * exporting into `metricsDir`. Each node exports to its own file, and a build that is interrupted
 * on one node is resumed and completed on whichever node steps up, so the observation for the
 * resumed build lands in a different file than the one for the interrupted build. Reading a single
 * file would miss it depending on which node flushed last.
 */
function durationCount(metricsDir, attrKey, attrValue) {
    let total = 0;
    for (const file of findOtelFilesWithSuffix(metricsDir)) {
        const record = readJsonlFile(file.name).at(-1);
        if (!record) {
            continue;
        }
        for (const metric of getFlatMetricsList(record)) {
            if (metric.name !== kDurationMetric) {
                continue;
            }
            for (const dataPoint of metric.histogram?.dataPoints ?? []) {
                const matches = (dataPoint.attributes ?? []).some(
                    (attr) => attr.key === attrKey && attr.value?.stringValue === attrValue,
                );
                if (matches) {
                    total += Number(dataPoint.count ?? 0);
                }
            }
        }
    }
    return total;
}

/**
 * Waits for the exported observation count to reach `expected`.
 */
function assertDurationCountSoon(metricsDir, attrKey, attrValue, expected) {
    assert.soon(
        () => durationCount(metricsDir, attrKey, attrValue) == expected,
        () =>
            `expected ${expected} '${attrValue}' duration observations in ${metricsDir}, found ` +
            durationCount(metricsDir, attrKey, attrValue),
    );
}

describe("index build completion duration metrics", function () {
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
        assert.eq(durationCount(this.metricsDir, "outcome", "success"), 0);

        assert.commandWorked(this.coll.createIndex({a: 1}));

        assertDurationCountSoon(this.metricsDir, "outcome", "success", 1);
    });

    it("failed index build", () => {
        // Fail a unique index build by inserting duplicate keys.
        assert.commandWorked(this.coll.insert([{a: 1}, {a: 1}]));
        assert.eq(durationCount(this.metricsDir, "outcome", "failure"), 0);

        assert.commandFailedWithCode(
            this.coll.createIndex({a: 1}, {unique: true}),
            ErrorCodes.DuplicateKey,
        );

        assertDurationCountSoon(this.metricsDir, "outcome", "failure", 1);
    });
});

describe("resumed index build duration metrics", function () {
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

        // The resumed build completes on whichever node steps up, so its observation lands in that
        // node's file rather than the original primary's. durationCount() reads both.
        assert.eq(durationCount(this.rst._pdibMetricsDir, "start_phase", "collection scan"), 0);

        // Resume an index build from the middle. Arbitrary, this was just to choose one resume to
        // test.
        PrimaryDrivenResumableIndexBuildTest.run(this.rst, {
            phase: PdibPhase.SCAN,
            positions: [PdibPosition.MIDDLE],
        });

        assertDurationCountSoon(this.rst._pdibMetricsDir, "start_phase", "collection scan", 1);
        assertDurationCountSoon(this.rst._pdibMetricsDir, "outcome", "success", 1);
    });
});
