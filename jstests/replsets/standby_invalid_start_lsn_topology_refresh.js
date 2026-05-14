/**
 * SERVER-126531: When every log server rejects the requested LSN with INVALID_START_LSN or
 * START_LSN_TRUNCATED, the standby must refresh topology from CMS and retry instead of
 * fatalling. Also covers mid-seal restart: kill the standby while it is requesting a new
 * segment, restart, and verify the topology-refresh path completes.
 *
 * Pre-fix: fatal exit when no log server accepts the LSN.
 * Post-fix: standby refreshes topology, picks a new log server, and rejoins.
 *
 * @tags: [
 *   requires_persistence,
 *   featureFlagDisaggregatedStorage,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = "standby_invalid_start_lsn_topology_refresh";
const rst = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const standby = rst.getSecondaries()[0];
const primaryDb = primary.getDB("test");

assert.commandWorked(primaryDb.coll.insert([{_id: 0}, {_id: 1}]));
rst.awaitReplication();

// Force every log server to reject the standby's requested LSN.
const rejectFp = configureFailPoint(standby, "logServerRejectAllStartLsn", {
    reason: "INVALID_START_LSN",
});

// Mid-seal restart: kill the standby while it is mid-request for a new segment.
const standbyId = rst.getNodeId(standby);
rst.stop(standbyId, 9 /* SIGKILL */);
rst.restart(standbyId, {startClean: false});

// Post-fix the standby refreshes topology from CMS and retries; pre-fix it fatals here.
rst.awaitSecondaryNodes(null, [rst.nodes[standbyId]]);
rejectFp.off();

assert.commandWorked(primaryDb.coll.insert([{_id: 2}]));
rst.awaitReplication();
assert.eq(3, rst.nodes[standbyId].getDB("test").coll.find().itcount());
rst.stopSet();
