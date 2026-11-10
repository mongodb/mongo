/**
 * Replicates with snappy and the compression pipeline enabled, then checks that the pipeline
 * metrics show batches went through it.
 *
 * @tags: [requires_replication]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const pipelineParams = {
    internalReplPipelineCompressionEnabled: true,
    internalReplPipelineCompressionThreadPoolSize: 2,
    internalReplPipelineCompressionQueueDepth: 8,
    internalReplPipelineCompressionMaxPendingBytes: 64 * 1024 * 1024,
    // The test's replies are much smaller than the 4MB default; 4096 is the effective floor.
    internalReplPipelineCompressionMinResponseBytes: 4096,
    internalReplPipelineCompressionWakeAwaitData: true,
};

const rst = new ReplSetTest({name: jsTestName(), nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet({networkMessageCompressors: "snappy", setParameter: pipelineParams});
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

for (const node of [primary, secondary]) {
    const res = assert.commandWorked(
        node.adminCommand({
            getParameter: 1,
            internalReplPipelineCompressionEnabled: 1,
            internalReplPipelineCompressionThreadPoolSize: 1,
            internalReplPipelineCompressionQueueDepth: 1,
            internalReplPipelineCompressionMaxPendingBytes: 1,
            internalReplPipelineCompressionMinResponseBytes: 1,
        }),
    );
    assert.eq(true, res.internalReplPipelineCompressionEnabled, tojson(res));
    assert.eq(2, res.internalReplPipelineCompressionThreadPoolSize, tojson(res));
    assert.eq(8, res.internalReplPipelineCompressionQueueDepth, tojson(res));
    assert.eq(64 * 1024 * 1024, res.internalReplPipelineCompressionMaxPendingBytes, tojson(res));
    assert.eq(4096, res.internalReplPipelineCompressionMinResponseBytes, tojson(res));
}

rst.awaitSecondaryNodes();
rst.awaitReplication();

// Hit for any compressed exhaust reply on the primary, before the pipeline decision; the metrics
// below are what show the pipeline was used.
const fp = configureFailPoint(primary, "beforeCompressingExhaustResponse");

const coll = primary.getDB("repl_compression_pipeline").c;
const docs = [];
for (let i = 0; i < 50; ++i) {
    docs.push({
        _id: i,
        payload: "x".repeat(16 * 1024),
    });
}
assert.commandWorked(coll.insertMany(docs, {writeConcern: {w: 1}}));

assert(fp.wait({timesEntered: 1}), "did not observe compressed replication exhaust response");
fp.off();

assert.soon(
    () => secondary.getDB("repl_compression_pipeline").c.find().itcount() === docs.length,
    "secondary did not replicate all documents",
    60 * 1000,
);

// The pipeline must publish its serverStatus metrics on the sync source (the primary), and they
// must reflect that batches actually flowed: at least one submitted, at least one sent, and the
// compression-latency count advanced. The in-flight gauge must not be negative. compressFailures
// must be zero in a healthy run.
assert.soon(
    () => {
        const m = assert.commandWorked(primary.adminCommand({serverStatus: 1})).metrics;
        const p = m && m.network && m.network.replCompressionPipeline;
        if (!p) {
            return false;
        }
        jsTestLog("replCompressionPipeline metrics: " + tojson(p));
        return p.batchesSubmitted > 0 && p.batchesSent > 0 && p.compressLatencyCount > 0;
    },
    "replCompressionPipeline metrics did not reflect any pipelined batches",
    60 * 1000,
);

const p = assert.commandWorked(primary.adminCommand({serverStatus: 1}))
              .metrics.network.replCompressionPipeline;
assert.eq(0, p.compressFailures, "unexpected compression failures: " + tojson(p));
// batchesSent counts completed network writes, so it can't run ahead of batchesTaken.
assert.gte(p.batchesTaken, p.batchesSent, tojson(p));
assert.gte(p.rawQueueDepth, 0, "in-flight gauge went negative: " + tojson(p));
assert.gt(p.rawBytes, 0, tojson(p));
assert.gt(p.compressedBytes, 0, tojson(p));
// Either can be 0 if the getMore never parked while a batch was ready.
assert.gte(p.awaitDataWakes, 0, tojson(p));
assert.gte(p.awaitDataWakeMissedNoTarget, 0, tojson(p));

jsTestLog("replication compression pipeline JS test PASSED");
rst.stopSet();
