/**
 * Tests that OTel build info datapoint-level attributes are correctly exported as an
 * unchanging gauge in both JSONL and Prometheus formats. Storage engine attribute is
 * only expected for mongod, not mongos.
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    createMetricsDirectory,
    extractPrometheusMetricIntValue,
    extractPrometheusMetricLabels,
    findOtelFilesWithSuffix,
    getFlatMetricsList,
    getLatestMetrics,
    getLatestRawRecord,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getBuildInfoAttrs(metricsDir) {
    const record = getLatestRawRecord(metricsDir);

    for (const metric of getFlatMetricsList(record)) {
        if (metric.name !== "mongodb.build.info") continue;
        const attrs = metric.gauge?.dataPoints?.[0]?.attributes ?? [];
        return Object.fromEntries(attrs.map((a) => [a.key, a.value.stringValue]));
    }

    return null;
}

function verifyBuildInfoAttrs(attrs, conn, {hasStorageEngine = true} = {}) {
    const adminDB = conn.getDB("admin");

    const expectedAttrs = {
        "name": adminDB.serverStatus().process,
        "instance_id": String(adminDB.serverStatus().pid.valueOf()),
        "version": adminDB.serverBuildInfo().version,
        "git_version": adminDB.serverBuildInfo().gitVersion,
        "storage_engine": hasStorageEngine ? adminDB.serverStatus().storageEngine.name : undefined,
    };

    for (const [key, expected] of Object.entries(expectedAttrs)) {
        assert.eq(attrs[key], expected, `attribute "${key}" mismatch`);
    }

    if (!hasStorageEngine) {
        assert(!attrs["storage_engine"], `expected attribute storage_engine to be absent`);
    }
}

describe("OTel build info attributes (mongod) (JSONL file export)", function () {
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

        // Wait until an export containing mongodb.build.info has occurred. The OTel exporter
        // starts before installMongodOtelMetrics() runs, so the first export may not yet contain
        // this metric on slow machines.
        assert.soon(
            () => {
                const attrs = getBuildInfoAttrs(this.metricsDir);
                if (attrs !== null) {
                    this.buildInfoAttrs = attrs;
                    return true;
                }
                return false;
            },
            "No mongodb.build.info metric in initial export",
            30000,
            1000,
        );
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("should include the correct build info attributes with exported metrics", function () {
        verifyBuildInfoAttrs(this.buildInfoAttrs, this.mongod);
    });

    it("should export mongodb.build.info gauge with value 1", function () {
        const metrics = getLatestMetrics(this.metricsDir);
        assert(metrics, "Expected metrics to be available");
        assert.eq(metrics["mongodb.build.info"]?.value, 1);
    });
});

describe("OTel build info attributes (mongod) (Prometheus file export)", function () {
    before(function () {
        this.metricsDir = createMetricsDirectory(jsTestName());
        this.metricsFilePath = this.metricsDir + "/" + "my-test-metrics.prom";
        this.mongod = MongoRunner.runMongod({
            setParameter: {
                openTelemetryExportIntervalMillis: 500,
                openTelemetryExportTimeoutMillis: 200,
                openTelemetryPrometheusMetricsPath: this.metricsFilePath,
            },
        });

        // Wait until an export containing mongodb.build.info has occurred. The OTel exporter
        // starts before installMongodOtelMetrics() runs, so the first export may not yet contain
        // this metric on slow machines.
        assert.soon(
            () => {
                const files = findOtelFilesWithSuffix(this.metricsDir, "my-test-metrics.prom");
                if (files.length === 0) return false;
                const content = cat(files[0].name);
                const attrs = extractPrometheusMetricLabels(content, "mongodb.build.info");
                if (attrs !== null) {
                    this.buildInfoAttrs = attrs;
                    this.buildInfoValue = extractPrometheusMetricIntValue(
                        content,
                        "mongodb.build.info",
                    );
                    return true;
                }
                return false;
            },
            "No mongodb.build.info metric in Prometheus export",
            30000,
            1000,
        );
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("should include the correct build info attributes with exported metrics", function () {
        assert(this.buildInfoAttrs, "Expected mongodb.build.info metric in Prometheus output");
        verifyBuildInfoAttrs(this.buildInfoAttrs, this.mongod);
    });

    it("should export mongodb.build.info gauge with value 1", function () {
        assert.eq(this.buildInfoValue, 1);
    });
});

describe("OTel build info attributes (mongos) (JSONL file export)", function () {
    before(function () {
        const {metricsDir, otelParams} = otelFileExportParams(jsTestName());

        this.metricsDir = metricsDir;
        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                mongosOptions: {
                    setParameter: {
                        ...otelParams,
                        openTelemetryExportIntervalMillis: 500,
                        openTelemetryExportTimeoutMillis: 200,
                    },
                },
            },
        });

        assert.soon(
            () => {
                const attrs = getBuildInfoAttrs(this.metricsDir);
                if (attrs !== null) {
                    this.buildInfoAttrs = attrs;
                    return true;
                }
                return false;
            },
            "No mongodb.build.info metric in initial export",
            30000,
            1000,
        );
    });

    after(function () {
        this.st.stop();
    });

    it("should include the correct build info attributes with exported metrics", function () {
        verifyBuildInfoAttrs(this.buildInfoAttrs, this.st.s, {hasStorageEngine: false});
    });

    it("should export mongodb.build.info gauge with value 1", function () {
        const metrics = getLatestMetrics(this.metricsDir);
        assert(metrics, "Expected metrics to be available");
        assert.eq(metrics["mongodb.build.info"]?.value, 1);
    });
});

describe("OTel build info attributes (mongos) (Prometheus file export)", function () {
    before(function () {
        this.metricsDir = createMetricsDirectory(jsTestName());
        this.metricsFilePath = this.metricsDir + "/" + "my-test-metrics.prom";
        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                mongosOptions: {
                    setParameter: {
                        openTelemetryExportIntervalMillis: 500,
                        openTelemetryExportTimeoutMillis: 200,
                        openTelemetryPrometheusMetricsPath: this.metricsFilePath,
                    },
                },
            },
        });

        assert.soon(
            () => {
                const files = findOtelFilesWithSuffix(this.metricsDir, "my-test-metrics.prom");
                if (files.length === 0) return false;
                const content = cat(files[0].name);
                const attrs = extractPrometheusMetricLabels(content, "mongodb.build.info");
                if (attrs !== null) {
                    this.buildInfoAttrs = attrs;
                    this.buildInfoValue = extractPrometheusMetricIntValue(
                        content,
                        "mongodb.build.info",
                    );
                    return true;
                }
                return false;
            },
            "No mongodb.build.info metric in Prometheus export",
            30000,
            1000,
        );
    });

    after(function () {
        this.st.stop();
    });

    it("should include the correct build info attributes with exported metrics", function () {
        assert(this.buildInfoAttrs, "Expected mongodb.build.info metric in Prometheus output");
        verifyBuildInfoAttrs(this.buildInfoAttrs, this.st.s, {hasStorageEngine: false});
    });

    it("should export mongodb.build.info gauge with value 1", function () {
        assert.eq(this.buildInfoValue, 1);
    });
});
