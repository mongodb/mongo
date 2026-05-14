/**
 * SERVER-97659: Reproduce the timing condition where the TTL monitor fires while the
 * node is part of a replica set but is NOT in a readable state (e.g. STARTUP2 during
 * initial sync). The very first check at the top of _doTTLSubPass:
 *
 *     if (isReplSet && !memberState.readable()) return false;
 *
 * must short-circuit the entire sub-pass: no documents are deleted and no per-index
 * work runs.
 *
 * Timing model:
 *   secondary starts initial sync ->
 *   initialSyncHangBeforeFinish pins it in STARTUP2 ->
 *   ttlMonitorSleepSecs=1 forces multiple TTL passes during the hang ->
 *   ttl.subPasses metric must advance, but coll.count() must remain unchanged.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 1}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB("test");
const coll = primaryDB.ttl_initial_sync;
coll.drop();

// Seed expired docs on the primary so the syncing node will replicate them and they
// would be eligible for TTL reaping if the monitor did not skip during initial sync.
const past = new Date(new Date().getTime() - 3600 * 1000 * 24);
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({x: past, i: i});
}
assert.commandWorked(bulk.execute({w: 1}));
assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 60}));

// Add a new node pinned in STARTUP2 via initialSyncHangBeforeFinish.
jsTestLog("Adding a node that will hang in initial sync (STARTUP2 / not readable)");
const syncingNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        ttlMonitorSleepSecs: 1,
        "failpoint.forceSyncSourceCandidate":
            tojson({mode: "alwaysOn", data: {hostAndPort: primary.host}}),
        "failpoint.initialSyncHangBeforeFinish": tojson({mode: "alwaysOn"}),
        numInitialSyncAttempts: 1,
    },
});
rst.reInitiate();
rst.waitForState(syncingNode, ReplSetTest.State.STARTUP_2);

// Wait until the syncing node is parked in STARTUP2 at the end-of-initial-sync hang
// failpoint. At this point the TTL monitor thread is running but every sub-pass must
// short-circuit because !memberState.readable() is true.
assert.commandWorked(syncingNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeFinish",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout,
}));

const syncingAdmin = syncingNode.getDB("admin");

// Confirm the node is unreadable.
const memberState = assert.commandWorked(syncingAdmin.runCommand({replSetGetStatus: 1})).myState;
assert.eq(ReplSetTest.State.STARTUP_2, memberState,
          "Syncing node must be in STARTUP2 for this test to be meaningful");

// Force the monitor to advance several sub-passes while not readable.
const subPassesBefore = syncingAdmin.serverStatus().metrics.ttl.subPasses;
const deletedDocsBefore = syncingAdmin.serverStatus().metrics.ttl.deletedDocuments;
assert.soon(
    () => syncingAdmin.serverStatus().metrics.ttl.subPasses >= subPassesBefore + 2,
    "TTL monitor did not advance sub-passes while in initial sync",
);

// While not readable, the monitor must NOT have deleted anything. The
// deletedDocuments counter is process-global and monotonic; assert no growth.
const deletedDocsAfter = syncingAdmin.serverStatus().metrics.ttl.deletedDocuments;
assert.eq(
    deletedDocsBefore,
    deletedDocsAfter,
    "TTL monitor must not delete documents while the node is not readable (STARTUP2)",
);

// Release initial sync and let the node finish syncing.
assert.commandWorked(
    syncingNode.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}),
);
rst.awaitSecondaryNodes(null, [syncingNode]);

rst.stopSet();
