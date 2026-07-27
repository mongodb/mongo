/**
 * Tests the openTelemetryTracingHttpExportHeaders server parameter. Validates startup parsing of
 * the CLI option and verifies configured headers are sent to the /v1/traces endpoint with OTLP
 * HTTP trace export requests.
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
import {enableFullSampling} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

describe("openTelemetryTracingHttpExportHeaders", function () {
    it("rejects invalid HTTP headers at startup", function () {
        assertRejectsInvalidHeadersAtStartup("openTelemetryTracingHttpExportHeaders");
    });

    describe("HTTP trace export", function () {
        before(function () {
            this.httpServer = new OtelHttpServer(jsTestName());
            this.httpServer.start();

            this.mongod = MongoRunner.runMongod({
                setParameter: {
                    opentelemetryHttpEndpoint: this.httpServer.getTracesEndpoint(),
                    openTelemetryTracingHttpExportHeaders: kCustomExportHeadersEncoded,
                    openTelemetryTracingBatchExportIntervalMillis: 500,
                    featureFlagOtelTraceSampling: true,
                },
            });

            // A standalone mongod does not sample commands by default, so force-sample ping.
            enableFullSampling(this.mongod, ["ping"]);

            // Generate a few root spans to export. The empty $traceCtx keeps the mongod the trace
            // entry point (the shell otherwise auto-injects a parent context).
            const db = this.mongod.getDB(jsTestName());
            for (let i = 0; i < 5; i++) {
                assert.commandWorked(db.runCommand({ping: 1, $traceCtx: {}}));
            }
        });

        after(function () {
            if (this.mongod) {
                MongoRunner.stopMongod(this.mongod);
            }
            if (this.httpServer) {
                this.httpServer.stop();
            }
        });

        it("sends configured custom headers to /v1/traces with trace export requests", function () {
            this.httpServer.waitForRequestWithHeaders(kCustomExportHeaders, {
                expectedPath: "/v1/traces",
            });
        });
    });
});
