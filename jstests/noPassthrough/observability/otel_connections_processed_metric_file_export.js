/**
 * Tests that the OpenTelemetry network.connections_processed counter metric is properly exported
 * to the file exporter when connections are established to both mongos and mongod in a sharded
 * cluster.
 *
 * This test verifies the end-to-end OTel metrics export flow:
 * 1. Configure a sharded cluster with file-based OTel metrics export for both mongos and mongod
 * 2. Get initial metric values before creating new connections
 * 3. Establish connections to both mongos and mongod which trigger the network.connections_processed
 *    counter
 * 4. Verify the metric appears correctly in the exported JSONL files for both components
 *
 * @tags: [requires_otel_build, requires_sharding]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    findMetricsFiles,
    readJsonlFile,
    findMetric,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Gets the current total value of the network.connections_processed metric from the metrics files in the given
 * directory that have been created after the provided date. Returns 0 if no metric is found.
 */
function getConnectionsMetricValue(metricsDir, afterDate) {
    let metricsFiles = [];
    assert.soon(
        () => {
            metricsFiles = findMetricsFiles(metricsDir, afterDate);
            return metricsFiles.length > 0;
        },
        `No recent metrics files found in ${metricsDir}`,
        30000,
        1000,
    );

    for (const file of metricsFiles) {
        const records = readJsonlFile(file.name);
        const foundMetric = findMetric(records, "network.connections_processed");
        if (foundMetric) {
            jsTest.log.info(`Found metric: ${tojson(foundMetric)}`);
            let totalValue = 0;
            for (const dataPoint of foundMetric.sum.dataPoints) {
                totalValue += dataPoint.asInt;
            }
            return totalValue;
        }
    }
    return 0;
}

/**
 * Gets the metric value before and after creating new connections and verifies the incremental count increases.
 */
function verifyConnectionsProcessedMetric(metricsDir, host, componentName) {
    // Because the setup commands may have created connections, get an initial value and just verify the increment is as
    // expected.
    const testCaseStartDate = new Date();
    // Create one new connection so we know new metrics will be exported.
    const conn = new Mongo(host);
    assert.commandWorked(conn.getDB("test").runCommand({ping: 1}));
    const initialValue = getConnectionsMetricValue(metricsDir, testCaseStartDate);
    jsTest.log.info(`Initial metric value: ${initialValue}`);

    const newConnections = 3;
    jsTest.log.info(`Creating ${newConnections} new connections to mongod...`);
    for (let i = 0; i < newConnections; i++) {
        const conn = new Mongo(host);
        assert.commandWorked(conn.getDB("test").runCommand({ping: 1}));
    }

    const expectedTotal = initialValue + newConnections;
    assert.soon(
        () => {
            // This may be greater than the expected total because of the initial connection we made and background
            // processes that may be creating connections.
            return getConnectionsMetricValue(metricsDir, testCaseStartDate) >= expectedTotal;
        },
        `${componentName} network.connections_processed counter should have recorded at least ${newConnections} ` +
            `new connections (initial: ${initialValue}, expected total: ${expectedTotal})`,
        30000,
        // Keep it short between retries so we are less likely to be reading a file at the same time as the metrics are
        // being written.
        300,
    );
}

describe("OTel network.connections_processed metric file export", function () {
    before(function () {
        Random.setRandomSeed();

        // Create separate metrics directories for mongos and mongod
        const testName = jsTestName();
        this.mongosMetricsDir = MongoRunner.toRealPath(`${testName}_mongos_otel_metrics_${Random.randInt(1000000)}`);
        assert(mkdir(this.mongosMetricsDir), `Failed to create mongos metrics directory: ${this.mongosMetricsDir}`);
        this.mongodMetricsDir = MongoRunner.toRealPath(`${testName}_mongod_otel_metrics_${Random.randInt(1000000)}`);
        assert(mkdir(this.mongodMetricsDir), `Failed to create mongod metrics directory: ${this.mongodMetricsDir}`);

        jsTest.log.info("Starting sharded cluster with OTel file exporter");
        jsTest.log.info(`Mongos metrics directory: ${this.mongosMetricsDir}`);
        jsTest.log.info(`Mongod metrics directory: ${this.mongodMetricsDir}`);

        // Common OTel setParameter config
        const otelConfig = {
            openTelemetryExportIntervalMillis: 500,
            openTelemetryExportTimeoutMillis: 200,
        };

        // Start sharded cluster with OTel metrics file exporter enabled on both mongos and shards
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

        this.mongos = this.st.s;
        this.mongod = this.st.rs0.getPrimary();

        // Run initial commands to ensure connections are established
        assert.commandWorked(this.mongos.getDB("test").runCommand({ping: 1}));
        assert.commandWorked(this.mongod.getDB("test").runCommand({ping: 1}));
    });

    after(function () {
        this.st.stop();
    });

    it("should correctly track new connections to mongos", function () {
        verifyConnectionsProcessedMetric(this.mongosMetricsDir, this.mongos.host, "mongos");
    });

    it("should correctly track new connections to mongod", function () {
        verifyConnectionsProcessedMetric(this.mongodMetricsDir, this.mongod.host, "mongod");
    });
});
