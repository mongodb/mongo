/**
 * Tests that a router (mongos) command produces a root tracing span, as an entry point for sharded
 * operations, exported via the OTel JSONL file exporter.
 *
 * @tags: [requires_otel_build, requires_sharding]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    enableFullSampling,
    getAllSpans,
    getEffectiveTraceDir,
    isRootSpan,
} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

describe("OTel router command root span file export", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: {
                setParameter: {
                    openTelemetryTracingFileFlushCount: 1,
                    opentelemetryTraceDirectory: MongoRunner.toRealPath("mongos_test_traces"),
                    openTelemetryTracingBatchExportIntervalMillis: 500,
                    featureFlagOtelTraceSampling: true,
                },
            },
        });

        enableFullSampling(this.st.s, {defaultSpans: true});

        this.db = this.st.s.getDB("test");

        // We can't use the trace dir above since it could be overwritten by resmoke.
        this.traceDir = getEffectiveTraceDir(this.st.s);
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
