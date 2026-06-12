/**
 * Smoke-tests that the serverStatus.opLatencies.latency OTel histogram is wired up and exported
 * via the JSONL file exporter. Unit tests (top_otel_test.cpp) cover per-op_type filtering, guard
 * logic, and bucket boundaries in detail; this test just confirms end-to-end recording works.
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertHistogramMetricIncreases,
    getLatestMetrics,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

function assertOpLatencyIncreases(metricsDir, opType, fn) {
    assertHistogramMetricIncreases({
        metricsDir,
        metricName: "serverStatus.opLatencies.latency",
        attrKey: "op_type",
        attrValue: opType,
        fn,
    });
}

describe("OTel serverStatus.opLatencies.latency histogram file export", function () {
    before(function () {
        const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
        this.metricsDir = metricsDir;

        this.mongod = MongoRunner.runMongod({
            setParameter: {
                ...otelParams,
                openTelemetryExportIntervalMillis: 500,
                openTelemetryExportTimeoutMillis: 200,
            },
        });

        this.db = this.mongod.getDB("test");
        this.coll = this.db.getCollection(jsTestName());

        assert.commandWorked(this.coll.insert({x: 1}));

        assert.soon(
            () => getLatestMetrics(metricsDir) !== null,
            "No initial metrics export",
            30000,
            500,
        );
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("increments on read operations", function () {
        assertOpLatencyIncreases(this.metricsDir, "read", () => this.coll.find({x: 1}).toArray());
    });

    it("increments on write operations", function () {
        assertOpLatencyIncreases(this.metricsDir, "write", () =>
            assert.commandWorked(this.coll.insert({y: 2})),
        );
    });

    it("increments on command operations", function () {
        assertOpLatencyIncreases(this.metricsDir, "command", () =>
            assert.commandWorked(this.db.runCommand({ping: 1})),
        );
    });
});
