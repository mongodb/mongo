/**
 * Tests that the query performance counter OTel metrics are exported only on mongod (shard), not on
 * mongos, in a sharded cluster. These counters are incremented in InShard::record() and should not
 * appear on mongos.
 *
 * Metrics tested:
 *   - serverStatus.metrics.queryExecutor.scanned (index keys examined)
 *   - serverStatus.metrics.queryExecutor.scannedObjects (documents examined)
 *   - serverStatus.metrics.document.returned (documents returned)
 *
 * @tags: [requires_otel_build, requires_sharding]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertCounterMetricIncreases,
    awaitMetrics,
    createMetricsDirectory,
    getLatestMetrics,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

describe("OTel query performance counters in sharded cluster", function () {
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

        // Insert test documents via mongos.
        const bulk = this.coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 10; i++) {
            bulk.insert({x: i, y: "value_" + i});
        }
        assert.commandWorked(bulk.execute());

        // Create an index to enable index-scan queries.
        assert.commandWorked(this.coll.createIndex({x: 1}));

        // Wait for initial metrics to be exported on both mongos and mongod.
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

    it("scanned increments on shard for index-scan queries", function () {
        assertCounterMetricIncreases({
            metricsDir: this.mongodMetricsDir,
            metricName: "serverStatus.metrics.queryExecutor.scanned",
            minIncrease: 1,
            fn: () => this.coll.find({x: {$gte: 0, $lt: 5}}).toArray(),
        });
    });

    it("scannedObjects increments on shard for collection-scan queries", function () {
        assertCounterMetricIncreases({
            metricsDir: this.mongodMetricsDir,
            metricName: "serverStatus.metrics.queryExecutor.scannedObjects",
            minIncrease: 1,
            fn: () => this.coll.find({y: "value_0"}).toArray(),
        });
    });

    it("returned increments on shard for queries", function () {
        assertCounterMetricIncreases({
            metricsDir: this.mongodMetricsDir,
            metricName: "serverStatus.metrics.document.returned",
            minIncrease: 1,
            fn: () => this.coll.find({x: {$gte: 0}}).toArray(),
        });
    });

    it("query perf counters are not exported on mongos", function () {
        const testStart = new Date();
        // Run some queries through mongos to give it a chance to increment counters.
        this.coll.find({x: 1}).toArray();
        this.coll.find({y: "value_0"}).toArray();

        // Wait for a snapshot newer than testStart so we know it reflects these queries.
        const mongosMetrics = awaitMetrics(
            this.mongosMetricsDir,
            testStart,
            () => true,
            () => "fresh mongos metrics snapshot",
        );
        const counterNames = [
            "serverStatus.metrics.queryExecutor.scanned",
            "serverStatus.metrics.queryExecutor.scannedObjects",
            "serverStatus.metrics.document.returned",
        ];
        for (const name of counterNames) {
            assert.eq(
                undefined,
                mongosMetrics?.[name],
                "Expected counter to be absent from mongos metrics",
                {name},
            );
        }
    });
});
