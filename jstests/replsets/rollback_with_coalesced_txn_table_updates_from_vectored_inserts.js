/**
 * Tests that the rollback procedure will update the 'config.transactions' table to be consistent
 * with the node data at the 'stableTimestamp', specifically in the case where multiple derived ops
 * to the 'config.transactions' table were coalesced into a single operation when performing
 * vectored inserts on the primary.
 *
 * @tags: [requires_persistence]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const rst = new ReplSetTest({
    nodes: {
        // Set the syncdelay to 1s to speed up checkpointing.
        n0: {syncdelay: 1},
        // Set the bgSyncOplogFetcherBatchSize to 1 oplog entry to guarantee replication progress
        // with the stopReplProducerOnDocument failpoint.
        n1: {setParameter: {bgSyncOplogFetcherBatchSize: 1}},
        n2: {setParameter: {bgSyncOplogFetcherBatchSize: 1}},
    },
    // Force secondaries to sync from the primary to guarantee replication progress with the
    // stopReplProducerOnDocument failpoint. Also disable primary catchup because some replicated
    // retryable write statements are intentionally not being made majority committed.
    settings: {chainingAllowed: false, catchUpTimeoutMillis: 0},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const ns = "test.retryable_write_coalesced_txn_updates";
assert.commandWorked(primary.getCollection(ns).insert({_id: -1}, {writeConcern: {w: 3}}));
// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
rst.awaitReplication();

const [secondary1, secondary2] = rst.getSecondaries();

// Disable replication partway into the retryable write on all of the secondaries. The idea is that
// while the primary will apply all of the writes in a single storage transaction, the secondaries
// will only apply up to insertBatchMajorityCommitted oplog entries.

let insertBatchTotal;
let insertBatchMajorityCommitted;
let stopReplProducerOnDocumentFailpoints;
let oplogFilterForMajority;

// When ReplicateVectoredInsertsTransactionally is enabled, inserts are batched into applyOps
// entries, so we need to insert more documents and stop on a different one.
if (FeatureFlagUtil.isPresentAndEnabled(primary, "ReplicateVectoredInsertsTransactionally")) {
    // Set the batch size to 2 so we're testing batching but don't have to insert a huge number
    // of documents
    assert.commandWorked(primary.adminCommand({setParameter: 1, internalInsertMaxBatchSize: 2}));
    // Using an odd number tests that the short batch (which should be an 'i', not an applyOps)
    // works.
    insertBatchTotal = 41;
    insertBatchMajorityCommitted = insertBatchTotal - 3;
    stopReplProducerOnDocumentFailpoints = [secondary1, secondary2].map(
        conn => configureFailPoint(conn, 'stopReplProducerOnDocument', {
            document: {"o.applyOps.o._id": insertBatchMajorityCommitted + 1}
        }));
    oplogFilterForMajority = {
        "o.applyOps.ns": ns,
        "o.applyOps.o._id": insertBatchMajorityCommitted
    };
} else {
    insertBatchTotal = 20;
    insertBatchMajorityCommitted = insertBatchTotal - 2;
    stopReplProducerOnDocumentFailpoints = [secondary1, secondary2].map(
        conn => configureFailPoint(conn,
                                   'stopReplProducerOnDocument',
                                   {document: {"_id": insertBatchMajorityCommitted + 1}}));
    oplogFilterForMajority = {ns: ns, "o._id": insertBatchMajorityCommitted};
}

const lsid = ({id: UUID()});

assert.commandWorked(primary.getCollection(ns).runCommand("insert", {
    documents: Array.from({length: insertBatchTotal}, (_, i) => ({_id: i})),
    lsid,
    txnNumber: NumberLong(1),
}));

const stmtMajorityCommitted =
    primary.getCollection("local.oplog.rs").findOne(oplogFilterForMajority);
assert.neq(null, stmtMajorityCommitted);

// Wait for the primary to have advanced its stable_timestamp.
assert.soon(() => {
    const {lastStableRecoveryTimestamp} =
        assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));

    const wtStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1})).wiredTiger;
    const latestMajority =
        wtStatus["snapshot-window-settings"]["latest majority snapshot timestamp available"];

    print(`${primary.host}: ${tojsononeline({
        lastStableRecoveryTimestamp,
        stmtMajorityCommittedTimestamp: stmtMajorityCommitted.ts,
        "latest majority snapshot timestamp available": latestMajority
    })}`);

    // Make sure 'secondary1' has a 'lastApplied' optime equal to 'stmtMajorityCommitted.ts'.
    // Otherwise, it can fail to win the election later.
    const {optimes: {appliedOpTime}} =
        assert.commandWorked(secondary1.adminCommand({replSetGetStatus: 1}));
    print(`${secondary1.host}: ${tojsononeline({appliedOpTime})}`);

    return bsonWoCompare(lastStableRecoveryTimestamp, stmtMajorityCommitted.ts) >= 0 &&
        bsonWoCompare(appliedOpTime.ts, stmtMajorityCommitted.ts) >= 0;
});

// Step up one of the secondaries and do a write which becomes majority committed to force the
// current primary to go into rollback.
assert.commandWorked(secondary1.adminCommand({replSetStepUp: 1}));
rst.freeze(primary);
rst.awaitNodesAgreeOnPrimary(undefined, undefined, secondary1);

rst.getPrimary();  // Wait for secondary1 to be a writable primary.

// Do a write which becomes majority committed and wait for the old primary to have completed its
// rollback.
assert.commandWorked(secondary1.getCollection("test.dummy").insert({}, {writeConcern: {w: 3}}));

for (const fp of stopReplProducerOnDocumentFailpoints) {
    fp.off();
}

// Reconnect to the primary after it completes its rollback and step it up to be the primary again.
rst.awaitNodesAgreeOnPrimary(undefined, undefined, secondary1);
assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
rst.stepUp(primary);

print(`${primary.host} session txn record: ${
    tojson(primary.getCollection("config.transactions").findOne({"_id.id": lsid.id}))}`);

// Make sure we don't re-execute operations that have already been inserted by making sure we
// we don't get a 'DuplicateKeyError'.
assert.commandWorked(primary.getCollection(ns).runCommand("insert", {
    documents: Array.from({length: insertBatchTotal}, (_, i) => ({_id: i})),
    lsid,
    txnNumber: NumberLong(1),
    writeConcern: {w: 3},
}));

rst.stopSet();
