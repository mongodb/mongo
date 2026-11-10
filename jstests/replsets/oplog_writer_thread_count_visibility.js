/**
 * While the first range of a batch is held on a secondary using oplogWriterThreadCount, its
 * lastWritten must not advance into that batch and the batch's documents must not be visible.
 *
 * This does not check the storage-level oplog visibility timestamp.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kOplogWriterThreadCount = 4;

const rst = new ReplSetTest({
    name: "oplog_writer_visibility",
    // Majority writes still work while one secondary is stalled.
    nodes: 3,
    nodeOptions: {
        setParameter: {
            oplogWriterThreadCount: kOplogWriterThreadCount,
        },
    },
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

if (!FeatureFlagUtil.isEnabled(primary, "ReduceMajorityWriteLatency")) {
    jsTestLog(
        "featureFlagReduceMajorityWriteLatency is disabled; skipping multi-threaded oplog " +
        "writer visibility test.");
    rst.stopSet();
    quit();
}

const dbName = "test";
const collName = "oplog_writer_visibility";
const primaryColl = primary.getDB(dbName)[collName];

const victim = rst.getSecondaries()[0];
const otherSecondary = rst.getSecondaries()[1];

jsTestLog("Seeding initial data and ensuring all nodes are caught up.");
assert.commandWorked(primaryColl.insert({_id: "seed"}, {writeConcern: {w: 3}}));
rst.awaitReplication();

// Does not fall back to appliedOpTime, which trails lastWritten by design.
function getLastWritten(node) {
    const status = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
    assert(status.optimes && status.optimes.writtenOpTime,
           "replSetGetStatus did not report optimes.writtenOpTime: " + tojson(status));
    return status.optimes.writtenOpTime;
}

const holdWriter = configureFailPoint(victim, "rsSyncApplyStop");
let fp;
const bigString = "z".repeat(256 * 1024);
const kLoadDocs = 200;
let victimLastWrittenAtStall;
try {
    holdWriter.wait({maxTimeMS: 60 * 1000});
    // Exclude entries already buffered when the writer stopped. Check bytes as well as count.
    const initialBuffered =
        assert.commandWorked(victim.adminCommand({serverStatus: 1})).metrics.repl.buffer.write;
    fp = configureFailPoint(
        victim, "hangOplogWriterBeforeWritingOplogBatchRange", {beginOffset: 0});
    // Separate inserts produce separate oplog entries.
    for (let i = 0; i < kLoadDocs; ++i) {
        assert.commandWorked(
            primaryColl.insert({_id: i, payload: bigString}, {writeConcern: {w: 1}}));
    }
    assert.soon(() => {
        const buffered =
            assert.commandWorked(victim.adminCommand({serverStatus: 1})).metrics.repl.buffer.write;
        return buffered.count - initialBuffered.count >= kLoadDocs &&
            buffered.sizeBytes - initialBuffered.sizeBytes >= kLoadDocs * bigString.length;
    }, "writer buffer did not accumulate enough new load entries and bytes");
    holdWriter.off();
    fp.wait({maxTimeMS: 60 * 1000});

    victimLastWrittenAtStall = getLastWritten(victim);
    jsTestLog("Victim lastWritten when the range was held: " + tojson(victimLastWrittenAtStall));
    sleep(3000);
    victim.setSecondaryOk();

    const victimLastWrittenDuring = getLastWritten(victim);
    const primaryLastWritten = getLastWritten(primary);
    assert.eq(0,
              timestampCmp(victimLastWrittenDuring.ts, victimLastWrittenAtStall.ts),
              "victim's lastWritten advanced before all ranges were written");
    assert.neq(0,
               timestampCmp(victimLastWrittenDuring.ts, primaryLastWritten.ts),
               "victim caught up while its first range was held");

    const visibleLoadDocs = victim.getDB(dbName)[collName].find({_id: {$lt: kLoadDocs}}).itcount();
    assert.eq(0, visibleLoadDocs, "victim applied documents from the unfinished batch");

    // The stalled secondary must not block majority writes.
    assert.commandWorked(
        primaryColl.insert({_id: "progress-check"}, {writeConcern: {w: 2, wtimeout: 60 * 1000}}));
} finally {
    if (fp) {
        fp.off();
    }
    holdWriter.off();
}
rst.awaitReplication();

jsTestLog("Verifying convergence and consistency across the set.");
const primaryCount = primaryColl.find().itcount();
for (const node of rst.nodes) {
    node.setSecondaryOk();
    assert.eq(primaryCount,
              node.getDB(dbName)[collName].find().itcount(),
              "node " + node.host + " has an inconsistent document count after the stall");
}

// checkOplogs verifies every node's oplog is a hole-free, identical, ordered sequence; a
// visibility violation that let a hole through would corrupt application and surface here.
rst.checkOplogs();
rst.checkReplicatedDataHashes();

// Sanity: the victim must have finalized the held batch and caught up now that it was released.
const victimLastWrittenAfter = getLastWritten(victim);
jsTestLog("Victim lastWritten after release: " + tojson(victimLastWrittenAfter));
assert.gt(timestampCmp(victimLastWrittenAfter.ts, victimLastWrittenAtStall.ts),
          0,
          "victim's lastWritten did not advance after releasing the held range");
assert.eq(kLoadDocs + 2,  // seed + kLoadDocs load docs + progress-check
          victim.getDB(dbName)[collName].find().itcount(),
          "victim did not converge to the full document set after releasing the held range");

jsTestLog("Done.");
rst.stopSet();
