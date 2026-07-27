/**
 * Tests the openTelemetryMetricsHttpExportHeaders server parameter. Validates startup parsing of
 * the CLI option and verifies configured headers are sent with OTLP HTTP metrics export requests.
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertRejectsInvalidHeadersAtStartup,
    kCustomExportHeaders,
    kCustomExportHeadersEncoded,
    OtelHttpServer,
} from "jstests/noPassthrough/observability/libs/otel_http_export_helpers.js";

describe("openTelemetryMetricsHttpExportHeaders", function () {
    it("rejects invalid HTTP headers at startup", function () {
        assertRejectsInvalidHeadersAtStartup("openTelemetryMetricsHttpExportHeaders");
    });

    describe("HTTP metrics export", function () {
        before(function () {
            this.httpServer = new OtelHttpServer(jsTestName());
            this.httpServer.start();

            this.mongod = MongoRunner.runMongod({
                setParameter: {
                    openTelemetryMetricsHttpEndpoint: this.httpServer.getMetricsEndpoint(),
                    openTelemetryMetricsHttpExportHeaders: kCustomExportHeadersEncoded,
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
            this.httpServer.waitForRequestWithHeaders(kCustomExportHeaders);
        });
    });
});
