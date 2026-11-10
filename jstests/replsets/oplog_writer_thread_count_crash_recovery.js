/**
 * A secondary using oplogWriterThreadCount is killed while an oplog batch has a hole: the first
 * range of the batch is held before writing, and later ranges of the same batch are already on
 * disk. Startup recovery must truncate after the batch's oplogTruncateAfterPoint, drop the
 * orphaned later ranges, and let the node rejoin with an oplog that matches the primary's.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const SIGKILL = 9;
const kOplogWriterThreadCount = 4;
// Accumulate 40MiB of payload so the batch exceeds the byte-based parallel threshold.
const kPayloadBytes = 1024 * 1024;
const kLoadDocs = 40;

const rst = new ReplSetTest({
    // Three nodes so the set keeps a primary and majority while one secondary is killed.
    nodes: 3,
    nodeOptions: {setParameter: {oplogWriterThreadCount: kOplogWriterThreadCount}},
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

if (!FeatureFlagUtil.isEnabled(primary, "ReduceMajorityWriteLatency")) {
    jsTestLog("featureFlagReduceMajorityWriteLatency is disabled; skipping test.");
    rst.stopSet();
    quit();
}

const dbName = "test";
const collName = "oplog_writer_crash_recovery";
const primaryColl = primary.getDB(dbName)[collName];

let victim = rst.getSecondaries()[0];
const victimId = rst.getNodeId(victim);

assert.commandWorked(primaryColl.insert({_id: "seed"}, {writeConcern: {w: 3}}));
rst.awaitReplication();

function getTruncateAfterPoint(node) {
    const doc = node.getDB("local")
                    .replset.oplogTruncateAfterPoint.find({_id: "oplogTruncateAfterPoint"})
                    .readConcern("local")
                    .toArray()[0];
    return doc ? doc.oplogTruncateAfterPoint : Timestamp(0, 0);
}

// With allowExternalReadsForReverseOplogScanRule enabled, a reverse scan reads the physical
// oplog without the secondary lastApplied bound, including committed later ranges of a held batch.
function getLoadOplogTimestamps(node) {
    return node.getDB("local")
        .oplog.rs.find({ns: dbName + "." + collName, op: "i", "o._id": {$gte: 0}}, {ts: 1})
        .sort({$natural: -1})
        .toArray()
        .map((e) => e.ts);
}

// Accumulate the load before the writer takes a batch, rather than stalling a one-entry batch.
const holdWriter = configureFailPoint(victim, "rsSyncApplyStop");
holdWriter.wait();
const bufferedBefore = assert.commandWorked(victim.adminCommand({serverStatus: 1})).metrics.repl.buffer.write;
const initialBufferedCount = bufferedBefore.count;
const initialBufferedBytes = bufferedBefore.sizeBytes;
const allowPhysicalReads = configureFailPoint(victim, "allowExternalReadsForReverseOplogScanRule");

// Hold the first range (beginOffset 0) of every batch before it writes anything.
const fp = configureFailPoint(victim, "hangOplogWriterBeforeWritingOplogBatchRange", {
    beginOffset: 0,
});

jsTestLog("Writing large documents so the victim forms a multi-range batch.");
const payload = "y".repeat(kPayloadBytes);
{
    const bulk = primaryColl.initializeOrderedBulkOp();
    for (let i = 0; i < kLoadDocs; i++) {
        bulk.insert({_id: i, payload: payload});
    }
    assert.commandWorked(bulk.execute({w: 1}));
}
const primaryLoadTs = primary.getDB("local")
                          .oplog.rs.find({ns: dbName + "." + collName, op: "i", "o._id": {$gte: 0}})
                          .sort({ts: 1})
                          .toArray()
                          .map((e) => e.ts);
assert.eq(kLoadDocs, primaryLoadTs.length);

assert.soon(() => {
    const buffered = assert.commandWorked(victim.adminCommand({serverStatus: 1})).metrics.repl.buffer.write;
    return buffered.count - initialBufferedCount >= kLoadDocs &&
        buffered.sizeBytes - initialBufferedBytes >= kLoadDocs * kPayloadBytes;
}, "victim did not buffer the load before starting its write batch");
holdWriter.off();
fp.wait();

// The held batch must have set a truncate point.
victim.setSecondaryOk();
const truncatePoint = getTruncateAfterPoint(victim);
assert.neq(0, timestampCmp(truncatePoint, Timestamp(0, 0)), "held batch did not set the truncate point");
jsTestLog("Truncate point of the held batch: " + tojson(truncatePoint));

// Wait until a later range of the held batch is on disk, so the oplog has a real hole.
let orphanTs;
assert.soon(() => {
    const written = getLoadOplogTimestamps(victim).filter((ts) => timestampCmp(ts, truncatePoint) > 0);
    if (written.length === 0) {
        return false;
    }
    orphanTs = written;
    return true;
}, "no later range of the held batch was written");

// The entry right after the truncate point belongs to the held first range and must be missing.
const firstAfterTruncate = primaryLoadTs.find((ts) => timestampCmp(ts, truncatePoint) > 0);
assert(firstAfterTruncate, "primary has no load entry after the truncate point");
assert(!orphanTs.some((ts) => timestampCmp(ts, firstAfterTruncate) === 0),
       "first range of the held batch was written; no hole to recover from");
jsTestLog("Hole confirmed: missing " + tojson(firstAfterTruncate) + ", orphaned later entries: " +
          orphanTs.length);

// Make the orphaned entries and the truncate point durable before the crash.
assert.commandWorked(victim.adminCommand({fsync: 1}));

allowPhysicalReads.off();
jsTestLog("SIGKILLing the victim while its oplog has a hole.");
rst.stop(victim, SIGKILL, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

rst.awaitReplication(undefined, undefined, rst.nodes.filter((n) => n.host !== victim.host));

// Pause the restarted writer before it can refill the oplog or set another batch's truncate point.
jsTestLog("Restarting the victim; recovery must truncate after the truncate point.");
victim = rst.start(victimId, {
    setParameter: {
        oplogWriterThreadCount: kOplogWriterThreadCount,
        "failpoint.rsSyncApplyStop": tojson({mode: "alwaysOn"}),
        "failpoint.allowExternalReadsForReverseOplogScanRule": tojson({mode: "alwaysOn"}),
    },
}, true /* restart */);
assert.commandWorked(victim.adminCommand({
    waitForFailPoint: "rsSyncApplyStop", timesEntered: 1, maxTimeMS: 60 * 1000,
}));
rst.awaitSecondaryNodes(undefined, [victim]);
victim.setSecondaryOk();

// Recovery may clamp the point to the stable timestamp, but must complete truncation.
checkLog.containsJson(victim, 21557);
checkLog.containsJson(victim, 21554);
assert.eq(0,
          timestampCmp(getTruncateAfterPoint(victim), Timestamp(0, 0)),
          "recovery did not clear the truncate point");
const afterRecoveryTs = getLoadOplogTimestamps(victim);
for (const ts of orphanTs) {
    assert(!afterRecoveryTs.some((recovered) => timestampCmp(recovered, ts) === 0),
           "recovery retained an orphaned later entry: " + tojson(ts));
}
assert.commandWorked(victim.adminCommand({
    configureFailPoint: "allowExternalReadsForReverseOplogScanRule", mode: "off",
}));
assert.commandWorked(victim.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));

const paramRes =
    assert.commandWorked(victim.adminCommand({getParameter: 1, oplogWriterThreadCount: 1}));
assert.eq(kOplogWriterThreadCount, paramRes.oplogWriterThreadCount);

rst.awaitReplication();

// Every load entry must now be present exactly once on the victim.
const recoveredTs = getLoadOplogTimestamps(victim).sort(timestampCmp);
assert.eq(primaryLoadTs.length, recoveredTs.length, "victim oplog has missing or extra load entries");
for (let i = 0; i < primaryLoadTs.length; i++) {
    assert.eq(0, timestampCmp(primaryLoadTs[i], recoveredTs[i]), "oplog mismatch at index " + i);
}

victim.setSecondaryOk();
assert.eq(primaryColl.find().itcount(), victim.getDB(dbName)[collName].find().itcount());

rst.checkOplogs();
rst.checkReplicatedDataHashes();
rst.stopSet();
