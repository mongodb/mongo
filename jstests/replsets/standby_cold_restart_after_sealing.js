/**
 * SERVER-126531: Verifies a standby survives a cold restart when the first log server it
 * connects to hosts a sealed segment. Cold startup seeds state from the checkpoint of that
 * server (_lastMajorityCommittedLSN = 0); segment scan must pick the segment using the larger
 * of the standby's committed LSN and the WT stable timestamp, so a stale checkpoint start does
 * not trigger restream-from-beginning and a 4017301 fassert.
 *
 * @tags: [
 *   requires_persistence,
 *   featureFlagDisaggregatedStorage,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = "standby_cold_restart_after_sealing";
const rst = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const standby = rst.getSecondaries()[0];
const primaryDb = primary.getDB("test");

assert.commandWorked(primaryDb.coll.insert([{_id: 0}, {_id: 1}, {_id: 2}]));
rst.awaitReplication();

// Force a segment seal on the log server, then drive new writes so the sealed segment is the
// one a fresh standby will pick from the checkpoint.
assert.commandWorked(primary.adminCommand({_logSegmentSeal: 1}));
assert.commandWorked(primaryDb.coll.insert([{_id: 3}, {_id: 4}]));
rst.awaitReplication();

const standbyId = rst.getNodeId(standby);
rst.stop(standbyId);
// Cold restart: _lastMajorityCommittedLSN = 0 on bootstrap.
rst.restart(standbyId, {startClean: false});
rst.awaitSecondaryNodes(null, [standby]);

// Pre-fix this fasserts 4017301 during catchup. Post-fix the standby reconciles via WT stable
// ts and does not restream redelivered entries.
assert.eq(5, standby.getDB("test").coll.find().itcount());
rst.stopSet();
