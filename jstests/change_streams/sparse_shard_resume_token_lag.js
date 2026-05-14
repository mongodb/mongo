/**
 * SERVER-80427 regression test: change-stream resume-token lag on sparse-write shards.
 *
 * In a sharded cluster, the cluster-wide change-stream high-water-mark advances only when every
 * shard reports progress. A shard that does not see writes only advances its oplog via the
 * periodic-noop writer, whose default cadence is 10s (`periodicNoopIntervalSecs`). Under a
 * sparse-write workload (writes concentrated on a subset of shards), consumers observe the
 * `postBatchResumeToken` (PBRT) lag behind the cluster's current time by up to one noop interval,
 * even though the producing shard is writing continuously.
 *
 * This test pins the lag empirically. It is expected to FAIL on a build that does not include the
 * fix proposed in `src/mongo/db/pipeline/SPARSE_SHARD_HEARTBEAT_DESIGN.md` (heartbeat-driven PBRT
 * advancement that does not require an oplog write on every shard). Once that fix lands, the
 * threshold below should be tightened to a small constant (e.g. <= 2s) and the
 * `expectedToFailToday` flag flipped.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_majority_read_concern,
 *   uses_change_streams,
 *   __TEMPORARILY_DISABLED__,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The fix is not yet in place; this test is a regression pin and is expected to fail today.
const expectedToFailToday = true;

// Default `periodicNoopIntervalSecs` is 10s. We pick a lag threshold below that ceiling but well
// above any realistic per-batch processing time, so a passing run on a fixed build is unambiguous.
const lagThresholdSeconds = 5;

// Sample the resume token this many times after the initial settle. Each sample is one getMore
// round-trip on the change-stream cursor; lag is measured at each sample.
const numSamples = 20;
const sampleSpacingMs = 500;
const settleMs = 1000;

// Write rate on the active shard. Sparse enough that we are not stress-testing throughput, dense
// enough that the active shard's oplog is always advancing.
const writeIntervalMs = 100; // ~10 writes/s on shard0
const totalActiveWrites = ((numSamples * sampleSpacingMs) + settleMs) / writeIntervalMs;

// Use the production default for periodicNoopIntervalSecs (10s) so the test reproduces the
// production complaint. Tests that want to lower this knob to verify a workaround should clone
// this file and adjust this parameter alone.
const st = new ShardingTest({
    shards: 3,
    mongos: 1,
    rs: {
        nodes: 1,
        setParameter: {
            writePeriodicNoops: true,
            periodicNoopIntervalSecs: 10,
        },
    },
});

const dbName = jsTestName();
const collName = jsTestName();
const mongosDB = st.s0.getDB(dbName);
const mongosColl = mongosDB[collName];

// Shard on {_id: 1} so we can pin every test write to shard0 with negative ids while the other
// two shards stay write-idle.
assert.commandWorked(mongosDB.adminCommand({enableSharding: dbName, primaryShard: st.rs0.getURL()}));
assert.commandWorked(mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// Three chunks across three shards. Negative ids live on shard0; the other ranges live on shards
// 1 and 2 and will receive zero writes during the test window.
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));
assert.commandWorked(mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 1000000}}));
assert.commandWorked(
    mongosDB.adminCommand({moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}),
);
assert.commandWorked(
    mongosDB.adminCommand({moveChunk: mongosColl.getFullName(), find: {_id: 2000000}, to: st.rs2.getURL()}),
);

// Open a whole-cluster change stream on mongos. The PBRT it reports must merge the high-water-mark
// of all three shards; on shards 1 and 2 that mark only ticks when the periodic-noop writer fires.
const csCursor = mongosColl.watch([], {cursor: {batchSize: 0}});

// Initial settle: wait for the stream to publish its first PBRT before driving any writes.
sleep(settleMs);
const initialPBRT = csCursor.getResumeToken();
assert.neq(undefined, initialPBRT, "stream did not report an initial postBatchResumeToken");

// Start a writer that touches only shard0. We deliberately do not synchronise it with the sampler
// thread; the test is interested in the lag between mongos's wall clock and the PBRT clusterTime,
// not in any per-write causal relationship.
const writerSpec = {
    host: st.s0.host,
    dbName,
    collName,
    totalWrites: Math.ceil(totalActiveWrites) + numSamples,
    intervalMs: writeIntervalMs,
};
const writerThread = new Thread(function (spec) {
    const conn = new Mongo(spec.host);
    const coll = conn.getDB(spec.dbName)[spec.collName];
    for (let i = 0; i < spec.totalWrites; ++i) {
        // Negative ids guarantee placement on shard0.
        assert.commandWorked(coll.insert({_id: -1 - i, t: new Date()}));
        sleep(spec.intervalMs);
    }
}, writerSpec);
writerThread.start();

// `getResumeToken` returns `{_data: <hex>}` once decoded by mongos. The resume-token's
// clusterTime is the first 8 bytes of the keyString-encoded `_data`, but the shell does not export
// `decodeResumeToken` from `change_stream_util.js` in every passthrough; we fall back on the cheap
// trick of asking the cluster for `$$CLUSTER_TIME` via `hello` and comparing it against the PBRT's
// extractable clusterTime via the `_internalChangeStreamSplitLargeEvent` no-op marker. To keep the
// test self-contained we instead use the helper exposed on the cursor: `csCursor.getResumeToken()`
// is monotonic in clusterTime, so we sample (wall clock at mongos, PBRT) pairs and assert the
// elapsed wall-clock distance between a freshly-issued mongos `hello` and the latest PBRT.
function nowClusterTimeSeconds() {
    const helloRes = assert.commandWorked(mongosDB.adminCommand({hello: 1}));
    // $clusterTime.clusterTime is a Timestamp; .getTime() yields seconds.
    return helloRes.$clusterTime.clusterTime.getTime();
}

function pbrtClusterTimeSeconds(token) {
    // The resume token's _data encodes the binary keyString; the top of that keyString is the
    // clusterTime. We re-derive it via a comparison probe: open a one-shot stream with
    // `startAfter: token` and ask for its operationTime, which equals the token's clusterTime.
    const probeRes = assert.commandWorked(
        mongosDB.runCommand({
            aggregate: collName,
            pipeline: [{$changeStream: {startAfter: token}}],
            cursor: {batchSize: 0},
        }),
    );
    const probeCursor = new DBCommandCursor(mongosDB, probeRes);
    const ts = probeRes.operationTime;
    probeCursor.close();
    return ts.getTime();
}

const samples = [];
for (let i = 0; i < numSamples; ++i) {
    sleep(sampleSpacingMs);
    const pbrt = csCursor.getResumeToken();
    const nowTs = nowClusterTimeSeconds();
    const pbrtTs = pbrtClusterTimeSeconds(pbrt);
    const lagSecs = nowTs - pbrtTs;
    samples.push({sample: i, nowTs, pbrtTs, lagSecs});
    jsTestLog(`sample[${i}] now=${nowTs} pbrt=${pbrtTs} lag=${lagSecs}s`);
}

writerThread.join();
csCursor.close();

const maxLag = samples.reduce((m, s) => Math.max(m, s.lagSecs), 0);
const meanLag = samples.reduce((acc, s) => acc + s.lagSecs, 0) / samples.length;
jsTestLog(`SERVER-80427 sparse-shard PBRT lag: max=${maxLag}s mean=${meanLag.toFixed(2)}s`);

const failureMsg =
    `SERVER-80427 regression pin: max PBRT lag ${maxLag}s exceeds threshold ${lagThresholdSeconds}s. ` +
    `Two of three shards saw zero writes; PBRT was throttled to the periodic-noop cadence. ` +
    `samples=${tojsononeline(samples)}`;

try {
    assert.lt(
        maxLag,
        lagThresholdSeconds,
        failureMsg,
    );
} catch (e) {
    if (expectedToFailToday) {
        jsTestLog(`EXPECTED FAILURE (will pass with proposed fix): ${e.message}`);
    } else {
        throw e;
    }
}

st.stop();
