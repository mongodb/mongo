/**
 * Tests the dedicated multi-threaded oplog writer controlled by the 'oplogWriterThreadCount'
 * startup parameter.
 *
 * When 'oplogWriterThreadCount' > 0 (and featureFlagReduceMajorityWriteLatency is enabled), the
 * steady-state OplogWriter on secondaries writes each oplog batch in parallel on a dedicated
 * worker pool ("OplogWriterWorker-*") instead of on a single thread. This test verifies the
 * end-to-end replication correctness of that path: everything written on the primary is
 * durably replicated and byte-for-byte identical on the secondary, including large documents
 * (which exercise the byte-aware batch splitting logic).
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kOplogWriterThreadCount = 4;

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            oplogWriterThreadCount: kOplogWriterThreadCount,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

// The parameter has no effect without this feature flag.
if (!FeatureFlagUtil.isEnabled(primary, "ReduceMajorityWriteLatency")) {
    jsTestLog(
        "featureFlagReduceMajorityWriteLatency is disabled; skipping multi-threaded oplog " +
        "writer test.");
    rst.stopSet();
    quit();
}

const paramRes = assert.commandWorked(
    secondary.adminCommand({getParameter: 1, oplogWriterThreadCount: 1}));
assert.eq(kOplogWriterThreadCount,
          paramRes.oplogWriterThreadCount,
          "oplogWriterThreadCount was not applied on the secondary");

const dbName = "test";
const collName = "oplog_writer_thread_count";
const primaryColl = primary.getDB(dbName)[collName];

jsTestLog("Inserting a batch of small documents.");
{
    const bulk = primaryColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 500; i++) {
        bulk.insert({_id: i, kind: "small", v: i});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));
}

jsTestLog("Inserting a batch of large documents to exercise byte-aware batch splitting.");
{
    // 50 documents with 256KiB payloads provide enough bytes for parallel range writes
    // when accumulated in the same writer batch.
    const bigString = "x".repeat(256 * 1024);
    const bulk = primaryColl.initializeUnorderedBulkOp();
    for (let i = 1000; i < 1050; i++) {
        bulk.insert({_id: i, kind: "large", payload: bigString});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));
}

jsTestLog("Performing updates and deletes to diversify the oplog batch content.");
assert.commandWorked(primaryColl.update({kind: "small"}, {$set: {touched: true}}, {multi: true}));
assert.commandWorked(primaryColl.remove({_id: {$gte: 1025, $lt: 1050}}));

// Ensure everything is durably replicated to the secondary.
rst.awaitReplication();

jsTestLog("Verifying the secondary's data matches the primary exactly.");
const secondaryColl = secondary.getDB(dbName)[collName];
secondary.setSecondaryOk();

// Document counts must match.
assert.eq(primaryColl.find().itcount(),
          secondaryColl.find().itcount(),
          "primary and secondary document counts differ");

// Only this collection's entries are compared, since the full oplogs legitimately differ.
const targetNs = `${dbName}.${collName}`;
const primaryOplog = primary.getDB("local").oplog.rs.find({ns: targetNs}).sort({$natural: 1}).toArray();
const secondaryOplog = secondary.getDB("local").oplog.rs.find({ns: targetNs}).sort({$natural: 1}).toArray();
assert.eq(primaryOplog.length,
          secondaryOplog.length,
          "primary and secondary user-collection oplog lengths differ");
for (let i = 0; i < primaryOplog.length; i++) {
    assert.eq(0,
              bsonWoCompare(primaryOplog[i], secondaryOplog[i]),
              () => `user-collection oplog entry ${i} differs between primary and secondary: ` +
                `primary=${tojson(primaryOplog[i])}, secondary=${tojson(secondaryOplog[i])}`);
}

// Use dbHash to assert the user collection is byte-for-byte identical across nodes.
const primaryHash =
    assert.commandWorked(primary.getDB(dbName).runCommand({dbHash: 1})).collections[collName];
const secondaryHash =
    assert.commandWorked(secondary.getDB(dbName).runCommand({dbHash: 1})).collections[collName];
assert.eq(primaryHash,
          secondaryHash,
          "collection dbHash differs between primary and secondary");

rst.stopSet();
