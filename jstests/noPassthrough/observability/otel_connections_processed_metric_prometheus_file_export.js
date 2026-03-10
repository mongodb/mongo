/**
 * Tests that the OpenTelemetry network.connections_processed counter metric is properly exported
 * to the metrics file in Prometheus format when connections are established to a mongod.
 *
 * This test verifies the end-to-end OTel metrics export flow:
 * 1. Configure a mongod with file-based Prometheus-format metrics export
 * 2. Get initial metric values before creating new connections
 * 3. Establish connections to mongod which trigger the network.connections_processed counter
 * 4. Verify the metric appears correctly in the exported text file
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    createMetricsDirectory,
    extractPrometheusMetricIntValue,
    extractPrometheusMetricTime,
    findMetricsFiles,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

/**
 * Gets the current total value of the network.connections_processed metric from the metrics files in the given
 * directory that have been created after the provided date. Returns 0 if no metric is found.
 */
function getConnectionsMetricValue(metricsDir, afterDate) {
    let metricsText;
    assert.soon(
        () => {
            let files = findMetricsFiles(metricsDir, /*fileSuffix=*/ "prometheus-metrics.txt");
            if (files.length === 0) {
                jsTest.log.info(`No metrics files found in ${metricsDir}`);
                return false;
            }
            assert.eq(files.length, 1, `Expected 1 metrics file, got ${files.length}`);
            metricsText = cat(files[0].name);
            return extractPrometheusMetricTime(metricsText) > afterDate.getTime();
        },
        `No recent metrics found in ${metricsDir}`,
        30000,
        1000,
    );

    return extractPrometheusMetricIntValue(metricsText, "network.connections_processed");
}

describe("OTel network.connections_processed metric Prometheus file export", function () {
    before(function () {
        this.metricsDir = createMetricsDirectory(jsTestName());

        jsTest.log.info("Starting mongod with OTel Prometheus file exporter");
        jsTest.log.info(`Metrics directory: ${this.metricsDir}`);

        this.mongod = MongoRunner.runMongod({
            setParameter: {
                openTelemetryExportIntervalMillis: 500,
                openTelemetryExportTimeoutMillis: 200,
                openTelemetryPrometheusMetricsDirectory: this.metricsDir,
            },
        });

        assert.commandWorked(this.mongod.getDB("test").runCommand({ping: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("should correctly track new connections to mongod", function () {
        // Because the setup commands may have created connections, get an initial value and just
        // verify the increment is as expected.
        const testCaseStartDate = new Date();
        // Create one new connection so we know new metrics will be exported.
        const conn = new Mongo(this.mongod.host);
        assert.commandWorked(conn.getDB("test").runCommand({ping: 1}));
        const initialValue = getConnectionsMetricValue(this.metricsDir, testCaseStartDate);
        jsTest.log.info(`Initial metric value: ${initialValue}`);

        const newConnections = 3;
        jsTest.log.info(`Creating ${newConnections} new connections to mongod...`);
        for (let i = 0; i < newConnections; i++) {
            const conn = new Mongo(this.mongod.host);
            assert.commandWorked(conn.getDB("test").runCommand({ping: 1}));
        }

        const expectedTotal = initialValue + newConnections;
        assert.soon(
            () => {
                // This may be greater than the expected total because of the initial connection we
                // made and background processes that may be creating connections.
                return getConnectionsMetricValue(this.metricsDir, testCaseStartDate) >= expectedTotal;
            },
            `mongod network.connections_processed counter should have recorded at least ` +
                `${newConnections} new connections (initial: ${initialValue}, expected total: ${expectedTotal})`,
            30000,
            300,
        );
    });
});
