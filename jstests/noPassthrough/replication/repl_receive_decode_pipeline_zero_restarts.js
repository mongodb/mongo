/**
 * Verifies receive/decode fallback across BackgroundSync fetcher replacement with zero query
 * restarts. The fallback must recover replication while decode failures remain enabled.
 *
 * @tags: [requires_replication]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function run(compressor) {
    const rst = new ReplSetTest({
        name: jsTestName() + "_" + compressor,
        nodes: [{}, {rsConfig: {priority: 0}}],
    });
    rst.startSet({
        networkMessageCompressors: compressor,
        setParameter: {
            internalReplReceiveDecodePipelineEnabled: true,
            internalReplReceiveDecodeThreadPoolSize: 4,
            internalReplReceiveDecodeQueueDepth: 1,
            internalReplReceiveDecodeMaxPendingBytes: 256 * 1024 * 1024,
            internalReplReceiveDecodeMaxConsecutiveFailures: 2,
            internalReplReceiveDecodeFallbackSeconds: 3600,
            oplogFetcherSteadyStateMaxFetcherRestarts: 0,
            bgSyncOplogFetcherBatchSize: 4,
        },
    });

    try {
        rst.initiate();
        const primary = rst.getPrimary();
        const secondary = rst.getSecondary();
        const dbName = "repl_receive_decode_pipeline_zero_restarts";
        const coll = primary.getDB(dbName).c;
        const stats = () => assert.commandWorked(secondary.adminCommand({serverStatus: 1}))
                                .metrics.repl.network.receiveDecode;

        function writeBatch(start) {
            const docs = [];
            for (let i = 0; i < 128; ++i) {
                docs.push({_id: start + i, payload: "receive-pipeline".repeat(4096)});
            }
            assert.commandWorked(coll.insertMany(docs, {writeConcern: {w: 1}}));
        }

        const params = assert.commandWorked(secondary.adminCommand({
            getParameter: 1,
            oplogFetcherSteadyStateMaxFetcherRestarts: 1,
            internalReplReceiveDecodeMaxConsecutiveFailures: 1,
        }));
        assert.eq(0, params.oplogFetcherSteadyStateMaxFetcherRestarts, tojson(params));
        assert.eq(2, params.internalReplReceiveDecodeMaxConsecutiveFailures, tojson(params));

        rst.awaitReplication();
        const initial = stats();
        writeBatch(0);
        rst.awaitReplication();
        assert.soon(() => stats().repliesDelivered > initial.repliesDelivered,
                    "steady-state replies did not use the receive pipeline");
        assert.eq(initial.failures, stats().failures, tojson(stats()));
        assert.eq(0, stats().fallbacks, tojson(stats()));

        // 21093 is a debug-level lifecycle log; enable it before taking the phase baseline.
        assert.commandWorked(secondary.adminCommand({
            setParameter: 1,
            logComponentVerbosity: {replication: {verbosity: 1}},
        }));
        let nextLogLine = assert.commandWorked(secondary.adminCommand({getLog: "global"}))
                              .totalLinesWritten;
        const phaseEvents = [];
        const eventIds = new Set([21092, 21093, 21119, 21274, 21275, 11369900]);

        function collectPhaseEvents() {
            const result = assert.commandWorked(secondary.adminCommand({getLog: "global"}));
            const firstRetainedLine = result.totalLinesWritten - result.log.length;
            assert.gte(result.totalLinesWritten, nextLogLine, "log position moved backwards");
            assert.lte(firstRetainedLine, nextLogLine, "fetcher observation logs were truncated");
            for (const line of result.log.slice(nextLogLine - firstRetainedLine)) {
                const entry = JSON.parse(line);
                if (eventIds.has(entry.id)) {
                    phaseEvents.push(entry);
                }
            }
            nextLogLine = result.totalLinesWritten;
        }

        const beforeFailure = stats();
        const failure = configureFailPoint(secondary, "replReceiveDecodeThrows");
        try {
            writeBatch(128);
            assert.soon(() => {
                collectPhaseEvents();
                return stats().fallbacks > beforeFailure.fallbacks;
            }, "receive pipeline did not fall back across fetcher replacement");
            rst.awaitReplication();
            collectPhaseEvents();

            // A exhausted its zero retry budget and stopped; BackgroundSync scheduled B, whose next
            // failure tripped the breaker. Cursor-only restarts cannot produce this lifecycle sequence.
            assert.eq([21274, 21093, 21092, 11369900],
                      phaseEvents.map(entry => entry.id),
                      tojson(phaseEvents));
            assert.eq(primary.host, phaseEvents[1].attr.syncSource, tojson(phaseEvents));
            assert.eq(primary.host, phaseEvents[2].attr.syncSource, tojson(phaseEvents));
            assert.eq(primary.host, phaseEvents[3].attr.syncSource, tojson(phaseEvents));
            assert.eq(2, phaseEvents[3].attr.consecutiveFailures, tojson(phaseEvents));
            assert.eq(beforeFailure.fallbacks + 1, stats().fallbacks, tojson(stats()));
            assert.gte(stats().failures, beforeFailure.failures + 2, tojson(stats()));

            const duringFallback = stats();
            writeBatch(256);
            rst.awaitReplication();
            assert.eq(duringFallback.failures, stats().failures, tojson(stats()));
            assert.eq(duringFallback.repliesDelivered, stats().repliesDelivered, tojson(stats()));
            assert.eq(duringFallback.fallbacks, stats().fallbacks, tojson(stats()));
            assert.eq(384, secondary.getDB(dbName).c.countDocuments({}));
            rst.checkReplicatedDataHashes(dbName);
            collectPhaseEvents();
            assert.eq([21274, 21093, 21092, 11369900],
                      phaseEvents.map(entry => entry.id),
                      "fetcher restarted again during fallback: " + tojson(phaseEvents));
        } finally {
            failure.off();
        }
    } finally {
        rst.stopSet();
    }
}

for (const compressor of ["snappy", "zstd", "zlib"]) {
    run(compressor);
}
