/**
 * Tests the openTelemetryMetricsHttpExportHeaders server parameter. Validates startup parsing of
 * the CLI option and verifies configured headers are sent with OTLP HTTP metrics export requests.
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createMetricsDirectory} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";
import {OtelMetricsHttpServer} from "jstests/noPassthrough/observability/libs/otel_metrics_http_export_helpers.js";

function tryStartMongod(setParameter) {
    clearRawMongoProgramOutput();
    const dbpath = MongoRunner.dataPath;
    resetDbpath(dbpath);

    const args = MongoRunner.arrOptions("mongod", {
        port: 0,
        dbpath: dbpath,
        setParameter: setParameter,
    });

    return runMongoProgram(...args);
}

describe("openTelemetryMetricsHttpExportHeaders", function () {
    it("rejects invalid HTTP headers at startup", function () {
        const exitCode = tryStartMongod({
            openTelemetryMetricsHttpExportHeaders: {"bad-header": 5},
        });
        assert.neq(exitCode, 0, "Expected mongod startup to fail for invalid header format");
        const output = rawMongoProgramOutput(".*");
        assert.gte(output.search(/strings or arrays of strings/), 0, output);
    });

    it("logs a warning when headers are configured without the HTTP exporter", function () {
        const metricsDir = createMetricsDirectory(jsTestName());
        const mongod = MongoRunner.runMongod({
            setParameter: {
                openTelemetryMetricsDirectory: metricsDir,
                openTelemetryMetricsHttpExportHeaders: {"Authorization": "Bearer ignored-token"},
            },
        });

        checkLog.containsJson(mongod, 12745900, {}, 30000);
        MongoRunner.stopMongod(mongod);
    });

    describe("HTTP metrics export", function () {
        const expectedHeaders = {
            "Authorization": "Bearer test-token",
            "X-Tenant-ID": "acme",
            "ValueWithComma": "combined,value",
        };

        before(function () {
            this.httpServer = new OtelMetricsHttpServer(jsTestName());
            this.httpServer.start();

            this.mongod = MongoRunner.runMongod({
                setParameter: {
                    openTelemetryMetricsHttpEndpoint: this.httpServer.getMetricsEndpoint(),
                    openTelemetryMetricsHttpExportHeaders: {
                        "Authorization": "Bearer test-token",
                        "X-Tenant-ID": "acme",
                        "ValueWithComma": "combined%2Cvalue",
                    },
                    openTelemetryExportIntervalMillis: 500,
                    openTelemetryExportTimeoutMillis: 200,
                },
            });

            assert.commandWorked(this.mongod.getDB(jsTestName()).runCommand({ping: 1}));
        });

        after(function () {
            if (this.mongod) {
                MongoRunner.stopMongod(this.mongod);
            }
            if (this.httpServer) {
                this.httpServer.stop();
            }
        });

        it("sends configured custom headers with metrics export requests", function () {
            this.httpServer.waitForRequestWithHeaders(expectedHeaders);
        });
    });
});
