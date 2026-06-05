/**
 * Tests that the serverStatus.opLatencies.latency OTel histogram is exported on both mongos and
 * mongod in a sharded cluster. ServiceLatencyTracker runs on every service, so the histogram
 * should increment on both.
 *
 * @tags: [requires_otel_build, requires_sharding]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertHistogramMetricIncreases,
    getLatestMetrics,
    createMetricsDirectory,
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

describe("OTel serverStatus.opLatencies.latency histogram in sharded cluster", function () {
    before(function () {
        this.mongosMetricsDir = createMetricsDirectory(jsTestName());
        this.mongodMetricsDir = createMetricsDirectory(jsTestName());

        const otelConfig = {
            openTelemetryExportIntervalMillis: 500,
            openTelemetryExportTimeoutMillis: 200,
        };

        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: {
                setParameter: {
                    ...otelConfig,
                    openTelemetryMetricsDirectory: this.mongosMetricsDir,
                },
            },
            rsOptions: {
                setParameter: {
                    ...otelConfig,
                    openTelemetryMetricsDirectory: this.mongodMetricsDir,
                },
            },
        });

        this.db = this.st.s.getDB("test");
        this.coll = this.db.getCollection(jsTestName());
        assert.commandWorked(this.coll.insert({x: 1}));

        assert.soon(
            () => getLatestMetrics(this.mongosMetricsDir) !== null,
            "No initial mongos metrics exported",
            30000,
            500,
        );
        assert.soon(
            () => getLatestMetrics(this.mongodMetricsDir) !== null,
            "No initial mongod metrics exported",
            30000,
            500,
        );
    });

    after(function () {
        this.st.stop();
    });

    it("increments on mongos and mongod for read operations", function () {
        const fn = () => this.coll.find({x: 1}).toArray();
        assertOpLatencyIncreases(this.mongosMetricsDir, "read", fn);
        assertOpLatencyIncreases(this.mongodMetricsDir, "read", fn);
    });

    it("increments on mongos and mongod for write operations", function () {
        const fn = () => assert.commandWorked(this.coll.insert({y: 2}));
        assertOpLatencyIncreases(this.mongosMetricsDir, "write", fn);
        assertOpLatencyIncreases(this.mongodMetricsDir, "write", fn);
    });

    it("increments on mongos and mongod for command operations", function () {
        const fn = () => assert.commandWorked(this.db.runCommand({ping: 1}));
        assertOpLatencyIncreases(this.mongosMetricsDir, "command", fn);
        assertOpLatencyIncreases(this.mongodMetricsDir, "command", fn);
    });
});
