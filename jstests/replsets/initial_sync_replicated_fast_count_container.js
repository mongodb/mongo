/**
 * Tests that initial sync correctly seeds the replicated fast count stores from a primary
 * running with featureFlagReplicatedFastCount and featureFlagContainerWrites enabled. After initial
 * sync, the secondary should:
 *   - Have the same fast count (count + dataSize) as the primary
 *   - Be able to apply ongoing container writes without ident-not-found errors
 *   - Serve the correct fast count from the persisted metadata store after a restart, when the
 *     in-memory count is reinitialized from persisted data rather than served from memory
 *
 * @tags: [
 *   featureFlagReplicatedFastCount,
 *   featureFlagContainerWrites,
 *   requires_replication,
 *   requires_persistence,
 *   requires_fsync,
 *   requires_fcv_90,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");

// Insert some documents and ensure fast count is being tracked on the primary.
const numDocs = 50;
const bulk = primaryDb.coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; ++i) {
    bulk.insert({_id: i, payload: "x".repeat(64)});
}
assert.commandWorked(bulk.execute());

// Force a flush so the primary persists its fast count metadata before the secondary syncs. Without
// this the donor might only hold the count in memory, and initial sync seeding would exercise the
// in-memory read path rather than seeding the secondary from the donor's persisted store.
assert.commandWorked(primaryDb.adminCommand({fsync: 1}));

const primaryStats = primaryDb.coll.stats();
jsTest.log.info("Primary stats", {primaryStats});
assert.eq(numDocs, primaryStats.count);

// Add a secondary that must perform initial sync against the primary.
const secondary = rst.add();
rst.reInitiate();
rst.awaitSecondaryNodes(null, [secondary]);
rst.awaitReplication();

// After initial sync, the secondary's view of the collection's count and dataSize must
// match the primary's.
const secondaryDb = secondary.getDB("test");
secondary.setSecondaryOk();

const secondaryStats = secondaryDb.coll.stats();
jsTest.log.info("Secondary stats post-initial-sync", {secondaryStats});
assert.eq(primaryStats.count, secondaryStats.count, "fast count mismatch after initial sync");
assert.eq(primaryStats.size, secondaryStats.size, "fast size mismatch after initial sync");

// Now write more on the primary; the secondary's fast count should converge via container
// write oplog application.
assert.commandWorked(primaryDb.coll.insert({_id: numDocs}));
rst.awaitReplication();

const convergedStats = primaryDb.coll.stats();
assert.eq(numDocs + 1, convergedStats.count);
assert.eq(numDocs + 1, secondaryDb.coll.stats().count);

// Restart the secondary so its in-memory fast count is discarded and reinitialized from the
// persisted metadata store. Reading the correct count after the restart gives a stronger signal
// that the replicated size and count info was synced and persisted correctly, rather than only
// living in memory.
const restartedSecondary = rst.restart(secondary);
rst.awaitSecondaryNodes(null, [restartedSecondary]);
restartedSecondary.setSecondaryOk();

const restartedSecondaryStats = restartedSecondary.getDB("test").coll.stats();
jsTest.log.info("Secondary stats post-restart", {restartedSecondaryStats});
assert.eq(
    convergedStats.count,
    restartedSecondaryStats.count,
    "fast count mismatch after restart (should be reinitialized from persisted metadata)",
);
assert.eq(
    convergedStats.size,
    restartedSecondaryStats.size,
    "fast size mismatch after restart (should be reinitialized from persisted metadata)",
);

rst.stopSet();
