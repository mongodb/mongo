/**
 * Tests that operations sent directly to a mongod (no mongos in front) produce tracing spans that
 * are exported by both the OTel JSONL file exporter and the OTLP HTTP exporter. Covers both a write
 * and a read command.
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {kSpanExporters} from "jstests/noPassthrough/observability/libs/otel_span_exporters.js";
import {
    enableFullSampling,
    getServiceInstanceId,
    isRootSpan,
    kNoSamplingStrategy,
    samplingConfigForStartup,
} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

for (const Exporter of kSpanExporters) {
    describe(`OTel direct-to-mongod span export (${Exporter.exporterName})`, function () {
        before(function () {
            this.exporter = new Exporter(jsTestName());
            this.exporter.start();

            this.mongod = MongoRunner.runMongod({
                setParameter: {
                    ...this.exporter.startupParams(),
                    openTelemetryTracingBatchExportIntervalMillis: 500,
                    featureFlagOtelTraceSampling: true,
                    openTelemetryTracingSampling: samplingConfigForStartup({
                        defaultSampling: kNoSamplingStrategy,
                    }),
                },
            });

            this.instanceId = getServiceInstanceId(this.mongod);

            this.db = this.mongod.getDB("test");
            this.coll = this.db.getCollection(jsTestName());

            // Enabling sampling on insert and find commands.
            enableFullSampling(this.mongod, {spanNames: ["insert", "find"]});
        });

        after(function () {
            MongoRunner.stopMongod(this.mongod);
            this.exporter.stop();
        });

        // Returns this mongod's spans that are trace roots with the given command name.
        function rootSpansForCommand(test, commandName) {
            return test.exporter
                .readSpans([test.mongod])
                .filter(
                    (span) =>
                        span.resource["service.instance.id"] === test.instanceId &&
                        isRootSpan(span) &&
                        span.name === commandName,
                );
        }

        it("exports a root span for a write command", function () {
            assert.soon(
                () => {
                    assert.commandWorked(
                        this.db.runCommand({
                            insert: this.coll.getName(),
                            documents: [{a: 1}],
                        }),
                    );
                    return rootSpansForCommand(this, "insert").length > 0;
                },
                "No root span named 'insert' was exported for a direct mongod write",
                30000,
                500,
            );
        });

        it("exports a root span for a read command", function () {
            assert.soon(
                () => {
                    assert.commandWorked(
                        this.db.runCommand({find: this.coll.getName(), filter: {}}),
                    );
                    return rootSpansForCommand(this, "find").length > 0;
                },
                "No root span named 'find' was exported for a direct mongod read",
                30000,
                500,
            );
        });
    });
}
