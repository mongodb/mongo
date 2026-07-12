/**
 * Tests that a router (mongos) command produces a root tracing span, as an entry point for sharded
 * operations, exported via the OTel JSONL file exporter.
 *
 * @tags: [requires_otel_build, requires_sharding]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAllSpans} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

function isRootSpan(span) {
    const parent = span.parentSpanId;
    return !parent || /^0*$/.test(parent);
}

describe("OTel router command root span file export", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: {
                setParameter: {
                    opentelemetryTraceDirectory: MongoRunner.toRealPath("mongos_test_traces"),
                    openTelemetryTracingBatchExportIntervalMillis: 500,
                    featureFlagOtelTraceSampling: true,
                },
            },
        });

        // Set the sampling config via setParameter rather than on the command line, which
        // stringifies values and breaks the numeric types the sampling config expects (e.g., doubles).
        assert.commandWorked(
            this.st.s.adminCommand({
                setParameter: 1,
                openTelemetryTracingSampling: {
                    defaultSampling: {
                        samplingFactor: 1.0,
                        tokenBucketRateLimit: {refillRate: 1000000, maxTokens: NumberInt(1000000)},
                    },
                },
            }),
        );

        this.db = this.st.s.getDB("test");

        // Determine the directory the mongos is actually exporting traces to, since resmoke may
        // override the value we set above.
        const res = assert.commandWorked(
            this.st.s.adminCommand({getParameter: 1, opentelemetryTraceDirectory: 1}),
        );
        this.traceDir = res.opentelemetryTraceDirectory;
        assert(this.traceDir, "mongos has no opentelemetryTraceDirectory configured");
    });

    after(function () {
        this.st.stop();
    });

    it("creates a root span for a mongos command", function () {
        const kCommandName = "ping";
        assert.soon(
            () => {
                // The empty $traceCtx is test-only: the shell auto-injects a parent trace
                // context, so we pass an empty one to keep mongos the trace entry point.
                assert.commandWorked(this.db.runCommand({[kCommandName]: 1, $traceCtx: {}}));
                return getAllSpans(this.traceDir).some(
                    (span) => isRootSpan(span) && span.name === kCommandName,
                );
            },
            `No root span named '${kCommandName}' was exported for a mongos command`,
            30000,
            500,
        );
    });
});
