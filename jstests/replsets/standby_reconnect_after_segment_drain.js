/**
 * SERVER-126531: After draining a segment and reconnecting, the standby must not call
 * clearLogServers() in a way that clears prevSegmentID and triggers restream-from-beginning.
 * Restream should only happen when both the old and new segment IDs are known AND differ.
 *
 * Pre-fix: reconnect redelivers entries older than what's on disk, fassert 4017301 fires when
 * the standby applies a redelivered oplog/checkpoint entry it has already applied (== check).
 * Post-fix: standby skips redelivered entries via <= comparison and survives the reconnect.
 *
 * @tags: [
 *   requires_persistence,
 *   featureFlagDisaggregatedStorage,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = "standby_reconnect_after_segment_drain";
const rst = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const standby = rst.getSecondaries()[0];
const primaryDb = primary.getDB("test");

assert.commandWorked(primaryDb.coll.insert([{_id: 0}, {_id: 1}]));
rst.awaitReplication();

// Drain the active segment on the standby, then drop the log-server connection so it
// reconnects with the prevSegmentID populated.
assert.commandWorked(standby.adminCommand({_logSegmentSeal: 1}));
const dropFp = configureFailPoint(standby, "dropLogServerConnection");
assert.commandWorked(primaryDb.coll.insert([{_id: 2}, {_id: 3}]));
rst.awaitReplication();
dropFp.off();

// Force one more sealed segment so the reconnect picks up a fresh segment header. The standby
// must compare old vs new segment IDs and NOT restream if they are equal.
assert.commandWorked(primary.adminCommand({_logSegmentSeal: 1}));
assert.commandWorked(primaryDb.coll.insert([{_id: 4}]));
rst.awaitReplication();

assert.eq(5, standby.getDB("test").coll.find().itcount());
// fassert 4017301 must not have fired during reconnect-redelivery.
assert(standby.adminCommand({hello: 1}).ok);
rst.stopSet();
