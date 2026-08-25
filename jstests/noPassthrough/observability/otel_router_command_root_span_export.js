/**
 * Tests that a router (mongos) command produces a root tracing span, as an entry point for sharded
 * operations, exported by both the OTel JSONL file exporter and the OTLP HTTP exporter.
 *
 * @tags: [requires_otel_build, requires_sharding]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {kSpanExporters} from "jstests/noPassthrough/observability/libs/otel_span_exporters.js";
import {
    enableFullSampling,
    isRootSpan,
} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

for (const Exporter of kSpanExporters) {
    describe(`OTel router command root span export (${Exporter.exporterName})`, function () {
        before(function () {
            this.exporter = new Exporter(jsTestName());
            this.exporter.start();

            this.st = new ShardingTest({
                shards: 1,
                mongos: 1,
                rs: {nodes: 1},
                mongosOptions: {
                    setParameter: {
                        openTelemetryTracingBatchExportIntervalMillis: 500,
                        featureFlagOtelTraceSampling: true,
                        ...this.exporter.startupParams(),
                    },
                },
            });

            enableFullSampling(this.st.s, {defaultSpans: true});

            this.db = this.st.s.getDB("test");
        });

        after(function () {
            this.st.stop();
            this.exporter.stop();
        });

        it("creates a root span for a mongos command", function () {
            const kCommandName = "ping";
            assert.soon(
                () => {
                    // The shell auto-injects a parent trace context, so skip it to keep mongos the
                    // trace entry point.
                    assert.commandWorked(
                        this.db.runCommand({[kCommandName]: 1}, {skipTelemetryContext: true}),
                    );
                    return this.exporter
                        .readSpans([this.st.s])
                        .some((span) => isRootSpan(span) && span.name === kCommandName);
                },
                `No root span named '${kCommandName}' was exported for a mongos command`,
                30000,
                500,
            );
        });
    });
}
