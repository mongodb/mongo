/**
 * Tests that initial sync produces a correct in-memory replicated fast count even when the sync
 * source's persisted fast count checkpoint lags behind its actual data.
 *
 * The scenario deliberately creates a "gap": the sync source flushes its fast count checkpoint at
 * `validAsOf` = T1 (count = N), then takes more writes (M docs) that are never persisted to its
 * fast count stores. During initial sync the secondary seeds its stores from the donor's *persisted*
 * checkpoint (N as of T1) and only applies oplog entries from `beginApplyingTimestamp` onward -- the
 * M docs land in the fetched-but-unapplied `validAsOf..beginApplyingTimestamp` range (SERVER-129848
 * clamps `beginFetchingTimestamp` down to `validAsOf`). The finalize step at the end of initial sync
 * (ReplicatedFastCountManager::finalizeMetadataFromInitialSync) re-derives the in-memory counts from
 * the persisted checkpoint plus every replayed oplog delta, recovering the missing M docs.
 *
 * @tags: [
 *   featureFlagReplicatedFastCount,
 *   featureFlagContainerWrites,
 *   requires_replication,
 *   requires_persistence,
 *   requires_fsync,
 *   # required failpoints won't exist on older binary versions
 *   requires_fcv_90,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
const primaryColl = primaryDb.coll;

// Insert the baseline documents and persist a fast count checkpoint for them. After the flush the
// donor's persisted fast count store holds count = kNumBaselineDocs as of some validAsOf timestamp.
const kNumBaselineDocs = 20;
for (let i = 0; i < kNumBaselineDocs; ++i) {
    assert.commandWorked(primaryColl.insert({_id: i, payload: "x".repeat(64)}));
}

{
    const sleepFp = configureFailPoint(primary, "sleepAfterFlush");
    assert.commandWorked(primaryDb.adminCommand({fsync: 1}));
    sleepFp.wait();
    sleepFp.off();
}

// Prevent the donor from persisting any further fast count deltas. failDuringFlush throws before the
// flusher acquires any locks (and is caught by the flush loop), so the donor keeps serving traffic
// and supporting initial sync while its persisted checkpoint stays pinned at the baseline.
const failFlushFp = configureFailPoint(primary, "failDuringFlush");

// These documents are reflected in the donor's data and oplog, but never in its persisted fast count
// store -- this is the gap the finalize step must close.
const kNumExtraDocs = 15;
const kNumTotalDocs = kNumBaselineDocs + kNumExtraDocs;
for (let i = kNumBaselineDocs; i < kNumTotalDocs; ++i) {
    assert.commandWorked(primaryColl.insert({_id: i, payload: "x".repeat(64)}));
}
// Trigger (and fail) a flush so we can be sure the extra docs are not persisted to the fast count
// store before the secondary harvests it.
assert.commandWorked(primaryDb.adminCommand({fsync: 1}));

const primaryCount = primaryColl.count();
const primaryDataSize = assert.commandWorked(
    primaryDb.runCommand({dataSize: primaryColl.getFullName()}),
).size;
assert.eq(kNumTotalDocs, primaryColl.find().itcount(), "primary actual count");
assert.eq(kNumTotalDocs, primaryCount, "primary fast count should reflect all docs");

// Add a secondary that must perform initial sync against the primary.
const secondary = rst.add();
rst.reInitiate();
rst.awaitSecondaryNodes(null, [secondary]);
rst.awaitReplication();

secondary.setSecondaryOk();
const secondaryDb = secondary.getDB("test");
const secondaryColl = secondaryDb.coll;

const secondaryCount = secondaryColl.count();
const secondaryDataSize = assert.commandWorked(
    secondaryDb.runCommand({dataSize: secondaryColl.getFullName()}),
).size;
jsTest.log.info("Post-initial-sync counts", {
    primaryCount,
    secondaryCount,
    secondaryActual: secondaryColl.find().itcount(),
});

// The finalize step must have re-derived the in-memory fast count from the persisted checkpoint plus
// the replayed oplog, so the secondary's fast count includes the extra docs even though the donor
// never persisted them in the metadata store.
assert.eq(
    kNumTotalDocs,
    secondaryColl.find().itcount(),
    "secondary actual count after initial sync",
);
assert.eq(
    primaryCount,
    secondaryCount,
    "secondary fast count must match primary after initial sync",
);
assert.eq(
    secondaryColl.find().itcount(),
    secondaryCount,
    "secondary fast count must match its actual count after initial sync",
);
assert.eq(primaryDataSize, secondaryDataSize, "secondary fast size must match primary");

failFlushFp.off();

// Sanity check: ongoing writes continue to keep the secondary's fast count in sync.
assert.commandWorked(primaryColl.insert({_id: kNumTotalDocs, payload: "x".repeat(64)}));
rst.awaitReplication();
assert.eq(kNumTotalDocs + 1, primaryColl.count());
assert.eq(kNumTotalDocs + 1, secondaryColl.count());

rst.stopSet();
