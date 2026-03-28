/**
 * Tests that the change_streams.cursor.lifespan OTel histogram metric is correctly exported to the
 * OTel JSONL file when change stream cursors are closed.
 *
 * Unlike serverStatus (which only exposes {average, count}), the file exporter provides the full
 * histogram structure including per-bucket counts, allowing verification that cursors with
 * different lifespans land in the correct buckets.
 *
 * The histogram bucket boundaries (in microseconds) are defined in clientcursor.cpp:
 *   bucket 0: (-inf,             1 000 000]  ≤ 1 second
 *   bucket 1: (1e6,             10 000 000]  1–10 seconds
 *   bucket 2: (10e6,       600 000 000 000]  10 seconds – 10 minutes
 *   bucket 3: (600e6,    1 200 000 000 000]  10–20 minutes
 *   bucket 4: (1200e6,   3 600 000 000 000]  20 minutes – 1 hour
 *   bucket 5: (3600e6,  86 400 000 000 000]  1 hour – 1 day
 *   bucket 6: (86400e6, 604 800 000 000 000] 1 day – 1 week
 *   bucket 7: (604800e6,              +inf)  > 1 week
 *
 * @tags: [requires_otel_build, requires_replication, uses_change_streams]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    createMetricsDirectory,
    getLatestMetrics,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

const kMetricName = "change_streams.cursor.lifespan";
// Number of buckets = number of boundaries + 1 (for the implicit +inf bucket).
const kNumBuckets = 8;

/**
 * Reads the change_streams.cursor.lifespan histogram from the latest exported metrics file.
 * Returns the raw OTel data point object, or null if the metric has not been exported yet.
 */
function readCSLifespanHistogram(metricsDir) {
    return getLatestMetrics(metricsDir)?.[kMetricName] ?? null;
}

/**
 * Returns a zeroed-out histogram baseline used when no data has been exported yet.
 */
function zeroBaseline() {
    return {count: "0", sum: 0, bucketCounts: Array(kNumBuckets).fill("0")};
}

/**
 * Polls until the metrics directory contains a snapshot newer than startDate whose histogram
 * satisfies pred(hist). Returns the histogram data point when the predicate is satisfied.
 */
function awaitCSLifespanHistogram(metricsDir, afterDate, expectedTotalCount) {
    let hist;
    assert.soon(
        () => {
            const metrics = getLatestMetrics(metricsDir);
            jsTest.log.debug("awaitCSLifespanHistogram", {metrics, afterDate: afterDate.getTime(), expectedTotalCount});
            if (
                metrics &&
                afterDate.getTime() < metrics.time &&
                metrics[kMetricName] !== undefined &&
                Number(metrics[kMetricName].count) >= expectedTotalCount
            ) {
                hist = metrics[kMetricName];
                return true;
            }
            return false;
        },
        `No recent metrics found in ${metricsDir}`,
        30000,
        500,
    );
    return hist;
}

describe("OTel change_streams.cursor.lifespan histogram file export", function () {
    before(function () {
        this.metricsDir = createMetricsDirectory(jsTestName());

        this.rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    openTelemetryExportIntervalMillis: 500,
                    openTelemetryExportTimeoutMillis: 200,
                    openTelemetryMetricsDirectory: this.metricsDir,
                },
            },
        });
        this.rst.startSet();
        this.rst.initiate();

        this.testColl = this.rst.getPrimary().getDB(jsTestName()).getCollection("coll");
        assert.commandWorked(this.testColl.insertOne({_id: 1}));
    });

    after(function () {
        this.rst.stopSet();
    });

    beforeEach(function () {
        // Capture cumulative baseline so each test can assert on deltas.
        this.baseline = readCSLifespanHistogram(this.metricsDir) ?? zeroBaseline();
        this.testStartDate = new Date();
    });

    it("records lifespan of short-lived cursors in bucket 0 (<= 1 second)", function () {
        const baseBucket0 = Number(this.baseline.bucketCounts[0]);
        const baseCount = Number(this.baseline.count);

        const numCursors = 3;
        for (let i = 0; i < numCursors; i++) {
            this.testColl.watch().close();
        }

        const csLifespanHist = awaitCSLifespanHistogram(this.metricsDir, this.testStartDate, baseCount + numCursors);

        assert.eq(Number(csLifespanHist.bucketCounts[0]), baseBucket0 + numCursors);
    });

    it("places a long-lived cursor in bucket 1 (1–10 s) and a short-lived cursor in bucket 0", function () {
        const baseBucket0 = Number(this.baseline.bucketCounts[0]);
        const baseBucket1 = Number(this.baseline.bucketCounts[1]);
        const baseSum = Number(this.baseline.sum);
        const baseCount = Number(this.baseline.count);

        // Open the long cursor before sleeping so its lifespan spans the sleep interval.
        const cursorLong = this.testColl.watch();
        sleep(1100); // lifespan > 1 000 000 µs → lands in bucket 1 (1–10 s)
        cursorLong.close();

        // Close immediately → lifespan ≈ 0 µs → lands in bucket 0 (≤ 1 s).
        this.testColl.watch().close();

        const csLifespanHist = awaitCSLifespanHistogram(this.metricsDir, this.testStartDate, baseCount + 2);

        assert.eq(Number(csLifespanHist.bucketCounts[0]), baseBucket0 + 1);
        assert.eq(Number(csLifespanHist.bucketCounts[1]), baseBucket1 + 1);
        assert.gte(
            csLifespanHist.sum,
            baseSum + 1e6,
            "cumulative sum must include the long cursor's lifespan (>= 1 000 000 µs)",
        );
    });
});
