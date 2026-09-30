/**
 * Tests that metrics are correctly collected from replication sockets.
 *
 * @tags: [requires_replication, requires_otel_build, multiversion_incompatible]
 */
import {describe, before, after, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isLinux} from "jstests/libs/server_security/os_helpers.js";
import {
    awaitMetrics,
    getLatestMetrics,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_metrics_file_export_helpers.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since socket information is only available on Linux platforms.");
    quit();
}

const kCommonPrefix = "mongodb.network.repl.secondary";
const kCongestionWindow = `${kCommonPrefix}.tcp_congestion_window_size`;
const kReceiveQueueBytes = `${kCommonPrefix}.receive_queue`;
const kReceiveQueueSize = `${kCommonPrefix}.receive_queue_size`;
const kCollectErrors = `${kCommonPrefix}.collect_errors`;
const kHistogramMetrics = [kCongestionWindow, kReceiveQueueSize, kReceiveQueueBytes];

describe("replication session TCP metrics", function () {
    let rs, primary, metricsDir, metrics;
    before(function () {
        const {metricsDir: dir, otelParams} = otelFileExportParams(jsTestName());
        metricsDir = dir;

        rs = new ReplSetTest({
            nodes: [{}, {rsConfig: {priority: 0}, setParameter: otelParams}],
        });
        rs.startSet();
        rs.initiate();
        // Block until set is fully operational
        rs.awaitSecondaryNodes();
        primary = rs.getPrimary();

        const start = new Date();
        assert.commandWorked(
            primary.getDB("test").foo.insert({payload: "a".repeat(1024)}, {writeConcern: {w: 2}}),
        );

        // Wait for metrics to be reported
        metrics = awaitMetrics(
            metricsDir,
            start,
            (m) => kHistogramMetrics.some((name) => Number(m[name]?.count ?? 0) > 0),
            (m) => `histogram counts: ${tojson(kHistogramMetrics.map((name) => m[name]?.count))}`,
        );
    });

    it("has all metrics populated", function () {
        for (const name of kHistogramMetrics) {
            assert.gt(metrics[name]?.count ?? 0, 0, `${name} missing from metrics report`);
        }
    });

    it("has sane values in each metric", function () {
        assert.gt(metrics[kCongestionWindow].min, 0, "Congestion window size should be positive");
        assert.gt(metrics[kReceiveQueueSize].min, 0, "Receive queue size should be positive");
        assert.gte(
            metrics[kReceiveQueueSize].min,
            0,
            "Bytes in receive queue should be non-negative",
        );
    });

    it("keeps sampling over time", function () {
        const start = new Date();
        for (const name of kHistogramMetrics) {
            const prevCount = Number(getLatestMetrics(metricsDir)[name].count);
            awaitMetrics(
                metricsDir,
                start,
                (m) => Number(m[name]?.count ?? 0) > prevCount,
                (m) => `${name}.count > ${prevCount} (got ${m[name]?.count})`,
            );
        }
    });

    /*
     * It is possible that a rare race condition could cause this to fail, but this should be very rare.
     * This could be guarded against by verifying that the next collection doesn't cause an error, but since this
     * seems highly unlikely, it is left out.
     */
    it("doesn't encounter collection errors", function () {
        const metrics = getLatestMetrics(metricsDir);
        assert.eq(
            Number(metrics[kCollectErrors]?.value ?? 0),
            0,
            "Number of errors should be zero",
        );
    });

    after(function () {
        if (rs) {
            rs.stopSet();
        }
    });
});
