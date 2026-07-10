/**
 * Tests that the OTel counter metrics for query performance counters
 * (serverStatus.metrics.queryExecutor.scanned, serverStatus.metrics.queryExecutor.scannedObjects,
 * and serverStatus.metrics.document.returned) are wired up and exported via the JSONL file
 * exporter.
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertCounterMetricIncreases,
    getLatestMetrics,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

describe("OTel query perf counter metrics file export", function () {
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

        // Insert 10 documents with an indexed field x and an unindexed field y.
        for (let i = 0; i < 10; i++) {
            assert.commandWorked(this.coll.insert({x: i, y: "value_" + i}));
        }
        assert.commandWorked(this.coll.createIndex({x: 1}));

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

    it("scanned increments on index-scan query", function () {
        assertCounterMetricIncreases({
            metricsDir: this.metricsDir,
            metricName: "mongodb.serverStatus.metrics.queryExecutor.scanned",
            minIncrease: 1,
            fn: () => this.coll.find({x: {$gte: 0}}).toArray(),
        });
    });

    it("scannedObjects increments on collection-scan query", function () {
        assertCounterMetricIncreases({
            metricsDir: this.metricsDir,
            metricName: "mongodb.serverStatus.metrics.queryExecutor.scannedObjects",
            minIncrease: 1,
            fn: () => this.coll.find({y: {$exists: true}}).toArray(),
        });
    });

    it("returned increments on any query", function () {
        assertCounterMetricIncreases({
            metricsDir: this.metricsDir,
            metricName: "mongodb.serverStatus.metrics.document.returned",
            minIncrease: 1,
            fn: () => this.coll.find({}).toArray(),
        });
    });
});
