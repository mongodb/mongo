/**
 * Tests the oplogFetchLagSeconds metric, which tracks how far behind the secondary's oplog
 * fetcher is from the primary. Verifies that the metric is ~0 when caught up, shows positive
 * lag when the secondary falls behind, and returns to ~0 after catching up.
 *
 * @tags: [
 *   tsan_incompatible,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}],
});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes(60000);

// Restart secondary with small batch size so it fetches one entry at a time.
// This must be done after init to avoid slowing down initial sync.
const secondary = rst.restart(1, {setParameter: {bgSyncOplogFetcherBatchSize: 1}});
rst.awaitSecondaryNodes(60000);

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const secondaryAdminDB = secondary.getDB("admin");

function getoplogFetcherLagSeconds() {
    return secondaryAdminDB.serverStatus().metrics.repl.network.oplogFetcherLagSeconds;
}

// Insert initial data and wait for replication to stabilize
assert.commandWorked(testDB.coll.insert({initial: true}));
rst.awaitReplication(60000);

// Verify the metric exists and is ~0 when caught up.
let ss = secondaryAdminDB.serverStatus();
assert(
    ss.metrics.repl.network.hasOwnProperty("oplogFetcherLagSeconds"),
    "oplogFetcherLagSeconds metric should exist in serverStatus",
);

let lagMetric = getoplogFetcherLagSeconds();
assert.gte(lagMetric, 0, "oplogFetcherLagSeconds should be non-negative");
// When caught up, lag should be 0 or at most 1 second
assert.lte(lagMetric, 1, "oplogFetcherLagSeconds should be ~0 when caught up");

// Create fetch lag by stopping fetcher, doing writes, then resuming.
const stopFetcher = configureFailPoint(secondary, "stopReplProducer");
stopFetcher.wait({maxTimeMS: 60000});

const bulk = testDB.coll.initializeUnorderedBulkOp();
for (let i = 0; i < 20; i++) {
    bulk.insert({lag_test: i});
}
assert.commandWorked(bulk.execute({w: 1}));

// Wait 3 seconds to ensure clear timestamp separation
sleep(3000);

// Do another write to ensure primary's lastApplied is in a new second
assert.commandWorked(testDB.coll.insert({final_write: true}, {writeConcern: {w: 1}}));

const hangAfterMetric = configureFailPoint(secondary, "hangOplogFetcherBeforeAdvancingLastFetched");
stopFetcher.off();

// Wait for the fetcher to fetch a batch and hit the hang point
hangAfterMetric.wait({maxTimeMS: 60000});

// Read the metric - should show positive lag
const lagWithHang = getoplogFetcherLagSeconds();

// Release the hang to let test continue
hangAfterMetric.off();
assert.gte(lagWithHang, 2, "Expected to observe fetch lag of at least 2 seconds, got: " + lagWithHang);

// Verify lag returns to ~0 after catching up.
rst.awaitReplication(60000);

lagMetric = getoplogFetcherLagSeconds();
assert.lte(lagMetric, 1, "oplogFetcherLagSeconds should return to ~0 after catchup");
rst.stopSet();
