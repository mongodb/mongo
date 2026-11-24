/**
 * Exercises Snappy oplog exhaust receive decoding, runtime fallback, and recovery after a decode
 * failure. Only the secondary's steady-state BackgroundSync opts into receive read-ahead.
 *
 * @tags: [requires_replication]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({name: jsTestName(), nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet({
    networkMessageCompressors: "snappy",
    setParameter: {
        internalReplReceiveDecodePipelineEnabled: true,
        internalReplReceiveDecodeThreadPoolSize: 4,
        internalReplReceiveDecodeQueueDepth: 4,
        internalReplReceiveDecodeMaxPendingBytes: 256 * 1024 * 1024,
        oplogFetcherSteadyStateMaxFetcherRestarts: 1,
        bgSyncOplogFetcherBatchSize: 4,
    },
});
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = "repl_receive_decode_pipeline";
const coll = primary.getDB(dbName).c;
const stats = () => assert.commandWorked(secondary.adminCommand({serverStatus: 1}))
                        .metrics.repl.network.receiveDecode;

function writeBatch(start) {
    const docs = [];
    for (let i = 0; i < 128; ++i) {
        docs.push({_id: start + i, payload: "snappy-pipeline".repeat(4096), sequence: start + i});
    }
    assert.commandWorked(coll.insertMany(docs, {writeConcern: {w: 1}}));
}

rst.awaitReplication();
const before = stats();
writeBatch(0);
rst.awaitReplication();
assert.soon(() => stats().repliesDelivered > before.repliesDelivered,
            "steady-state Snappy replies did not use the receive pipeline");
assert.eq(before.failures, stats().failures, tojson(stats()));
rst.checkReplicatedDataHashes(dbName);

// Disable while a worker is stuck on a reply. Disabling alone must end the fetcher's wait, without
// releasing the stuck decode first. The fetcher closes the old stream and issues a fresh find from
// its last successfully enqueued optime, never from its read-ahead position.
const hang = configureFailPoint(secondary, "hangReplReceiveDecode");
try {
    writeBatch(128);
    hang.wait();
    assert.commandWorked(secondary.adminCommand({
        setParameter: 1,
        internalReplReceiveDecodePipelineEnabled: false,
    }));
    rst.awaitReplication();
} finally {
    hang.off();
}
writeBatch(256);
rst.awaitReplication();
rst.checkReplicatedDataHashes(dbName);

assert.commandWorked(secondary.adminCommand({
    setParameter: 1,
    internalReplReceiveDecodePipelineEnabled: true,
}));
const beforeRecovery = stats();
writeBatch(384);
rst.awaitReplication();
assert.soon(() => stats().repliesDelivered > beforeRecovery.repliesDelivered,
            "receive pipeline did not resume after re-enabling");

// A failed worker must terminate its old stream. Inject exactly one failure so this phase cannot
// trip the fallback; a new stream then resumes without accepting later prefetched replies from the
// failed stream.
const beforeFailure = stats();
const fail = configureFailPoint(secondary, "replReceiveDecodeThrows", {}, {times: 1});
writeBatch(512);
fail.wait();
rst.awaitReplication();
assert.eq(640, secondary.getDB(dbName).c.countDocuments({}));
assert.eq(beforeFailure.failures + 1, stats().failures, tojson(stats()));
assert.eq(beforeFailure.fallbacks, stats().fallbacks, tojson(stats()));
rst.checkReplicatedDataHashes(dbName);
const beforeContinuedReplication = stats();
writeBatch(640);
rst.awaitReplication();
assert.soon(() => stats().repliesDelivered > beforeContinuedReplication.repliesDelivered,
            "receive pipeline did not resume after a single failure");
assert.eq(768, secondary.getDB(dbName).c.countDocuments({}));
rst.checkReplicatedDataHashes(dbName);

// A pipeline that keeps failing to start must also trip the breaker. Each restart's synchronous
// first batch resets the restart budget, so without counting start failures the fetcher would
// reconnect forever. This runs last because the fallback keeps the pipeline off afterwards.
assert.commandWorked(secondary.adminCommand({
    setParameter: 1,
    internalReplReceiveDecodePipelineEnabled: false,
    internalReplReceiveDecodeFallbackSeconds: 3600,
}));
writeBatch(768);
rst.awaitReplication();
assert.soon(() => {
    const current = stats();
    return current.activeDecodeTasks == 0 && current.pendingReplies == 0 &&
        current.pendingBytes == 0;
}, "old receive pipeline did not drain after disabling");
const beforeStartFailure = stats();
const startFailure = configureFailPoint(secondary, "replReceiveDecodeStartThrows");
try {
    assert.commandWorked(secondary.adminCommand({
        setParameter: 1,
        internalReplReceiveDecodePipelineEnabled: true,
    }));
    writeBatch(896);
    startFailure.wait();
    assert.soon(() => stats().fallbacks > beforeStartFailure.fallbacks,
                "persistent pipeline start failures did not trip the fallback");
    rst.awaitReplication();
    const duringFallback = stats();
    writeBatch(1024);
    rst.awaitReplication();
    assert.eq(duringFallback.fallbacks, stats().fallbacks, tojson(stats()));
    assert.eq(duringFallback.repliesDelivered, stats().repliesDelivered, tojson(stats()));
} finally {
    startFailure.off();
}
assert.eq(1152, secondary.getDB(dbName).c.countDocuments({}));
rst.checkReplicatedDataHashes(dbName);
rst.stopSet();
