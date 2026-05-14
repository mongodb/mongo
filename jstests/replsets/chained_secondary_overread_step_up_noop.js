/**
 * SERVER-120205 regression: a chained secondary tailing another secondary must
 * not be able to read the step-up no-op entry past the new primary's oplog
 * visibility timestamp.
 *
 * Companion to the TLA+ spec
 *   src/mongo/tla_plus/Replication/ChainedSecondaryOverRead/
 * which models the same bug at the protocol level.
 *
 * Topology:
 *   primary - secondary0 - secondary1
 *
 * secondary1 is forced to sync from secondary0. We hang secondary0's oplog
 * visibility thread (failpoint hangBeforeUpdatingVisibility), step secondary0
 * up to primary, then drive a no-op write at the new term. While the
 * visibility thread is paused on the new primary, secondary1's tailable oplog
 * cursor must NOT deliver the no-op entry: its lastApplied (and the last
 * oplog entry on secondary1) must remain at the previous term until the new
 * primary's visibility advances.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const name = jsTestName();
const rst = new ReplSetTest({
    name: name,
    // Speed up the no-op writer on secondary0 so the step-up no-op lands
    // promptly once we let writes resume.
    nodes: [{}, {setParameter: {periodicNoopIntervalSecs: 2}}, {}],
    settings: {chainingAllowed: true},
});
rst.startSet();
rst.initiate();

const [primary, secondary0, secondary1] = rst.nodes;
rst.waitForState(primary, ReplSetTest.State.PRIMARY);
rst.awaitReplication();

const testDB = "testDB";
const testColl = "testColl";

// Get secondary0 strictly ahead of secondary1 by pausing replication on
// secondary1 while we land one client write through the original primary.
stopServerReplication(secondary1);
assert.commandWorked(primary.getDB(testDB).getCollection(testColl).insert({a: 1}));
assert.soonNoExcept(() => secondary0.getDB(testDB).getCollection(testColl).findOne({a: 1}) !== null);

// Pin secondary1's sync source to secondary0 so it chains through it.
assert.commandWorked(secondary1.adminCommand({
    configureFailPoint: "forceSyncSourceCandidate",
    mode: "alwaysOn",
    data: {hostAndPort: secondary0.host},
}));

restartServerReplication(secondary1);
rst.awaitSyncSource(secondary1, secondary0);

// Hang the oplog visibility thread on secondary0 BEFORE it steps up so the
// step-up no-op entry will land in secondary0's oplog without visibility
// advancing past it.
jsTest.log.info("Arming hangBeforeUpdatingVisibility on secondary0");
const hangVis = configureFailPoint(secondary0, "hangBeforeUpdatingVisibility");

jsTest.log.info("Stepping secondary0 up");
assert.commandWorked(secondary0.adminCommand({replSetStepUp: 1}));

jsTest.log.info("Waiting for secondary0 to hit hangBeforeUpdatingVisibility");
hangVis.wait();

// Encourage the periodic no-op writer to actually emit a no-op now that
// secondary0 is the new primary.
assert.commandWorked(secondary0.adminCommand({setParameter: 1, writePeriodicNoops: true}));

const newPrimaryVis = secondary0.getDB("admin").serverStatus().wiredTiger.oplog;
const chainedVis = secondary1.getDB("admin").serverStatus().wiredTiger.oplog;
jsTest.log.info("new primary visibility: " + tojson(newPrimaryVis));
jsTest.log.info("chained secondary visibility: " + tojson(chainedVis));

// Invariant 1: chained secondary's visibility never exceeds new primary's
// visibility.
assert.lte(timestampCmp(chainedVis["visibility timestamp"], newPrimaryVis["visibility timestamp"]), 0,
           "chained secondary visibility advanced past new primary's");

// Invariant 2: chained secondary has not yet replicated any term-2 entry
// (the step-up no-op) because the new primary's visibility has not advanced
// past it.
const lastEntry = secondary1.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1)[0];
jsTest.log.info("last oplog entry on chained secondary: " + tojson(lastEntry));
assert.eq(lastEntry.t, 1,
          "chained secondary read past new primary's visibility (SERVER-120205)");

// Sanity: both nodes already agree on the new term.
const primStatus = secondary0.adminCommand({replSetGetStatus: 1});
const secStatus = secondary1.adminCommand({replSetGetStatus: 1});
assert.eq(primStatus.term, 2);
assert.eq(secStatus.term, 2);

// Release the visibility thread and let the cluster converge.
hangVis.off();

rst.awaitNodesAgreeOnPrimary(rst.timeoutMS, [primary, secondary0, secondary1], secondary0);
rst.awaitReplication();
rst.stopSet();
