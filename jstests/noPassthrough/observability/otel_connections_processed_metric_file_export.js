/**
 * Tests that the OpenTelemetry network.connections_processed counter metric is properly exported
 * to the file exporter when a connection is established.
 *
 * This test verifies the end-to-end OTel metrics export flow:
 * 1. Configure mongod with file-based OTel metrics export
 * 2. Establish connections which trigger the network.connections_processed counter
 * 3. Verify the metric appears in the exported JSONL file
 *
 * @tags: [requires_otel_build]
 */

import {
    findMetricsFiles,
    readJsonlFile,
    findMetric,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";
import {isLinux} from "jstests/libs/os_helpers.js";

// OTel Metrics are only enabled on Linux for now.
if (!isLinux()) {
    quit();
}

const testName = jsTestName();
Random.setRandomSeed(0);
const metricsDir = MongoRunner.toRealPath(testName + "_otel_metrics_" + Random.randInt(1000000));

// Create the metrics directory
assert(mkdir(metricsDir), "Failed to create metrics directory: " + metricsDir);

jsTest.log.info("Starting mongod with OTel file exporter, metrics directory: " + metricsDir);

// Start mongod with OTel metrics file exporter enabled
// Use a short export interval to avoid long test times
const mongod = MongoRunner.runMongod({
    setParameter: {
        featureFlagOtelMetrics: true,
        openTelemetryMetricsDirectory: metricsDir,
        openTelemetryExportIntervalMillis: 500,
        openTelemetryExportTimeoutMillis: 200,
    },
});
assert.neq(null, mongod, "mongod was unable to start up with OTel metrics configuration");

// Run a simple command to ensure the connection is established and processed
// This will trigger the network.connections_processed counter increment
const testDB = mongod.getDB("test");
assert.commandWorked(testDB.runCommand({ping: 1}));

// Create additional connections to ensure we have measurable activity
const newConnections = 3;
for (let i = 0; i < newConnections; i++) {
    const conn = new Mongo(mongod.host);
    assert.commandWorked(conn.getDB("test").runCommand({ping: 1}));
}

assert.soon(
    () => {
        const metricsFiles = findMetricsFiles(metricsDir);
        if (metricsFiles.length === 0) {
            jsTest.log.info("No metrics files found yet in: " + metricsDir);
            return false;
        }

        jsTest.log.info("Found " + metricsFiles.length + " metrics file(s)");

        for (const file of metricsFiles) {
            jsTest.log.info("Checking metrics file: " + file.name);
            const records = readJsonlFile(file.name);
            jsTest.log.info("Found " + records.length + " record(s) in file");

            const foundMetric = findMetric(records, "network.connections_processed");
            if (foundMetric) {
                jsTest.log.info("Found network.connections_processed metric: " + tojson(foundMetric));
                let totalValue = 0;
                for (const dataPoint of foundMetric.sum.dataPoints) {
                    totalValue += dataPoint.asInt;
                }

                jsTest.log.info("Total network.connections_processed value: " + totalValue);
                return totalValue == newConnections + 1;
            }
        }

        return false;
    },
    `network.connections_processed counter should have recorded ${newConnections + 1} connections`,
    30000,
    1000,
);

MongoRunner.stopMongod(mongod);
