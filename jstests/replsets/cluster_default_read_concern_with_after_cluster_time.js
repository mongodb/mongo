/**
 * SERVER-126299: Cluster-wide default read concern is not honored when
 * afterClusterTime is sent without an explicit level.
 *
 * Reproduces the bug as follows:
 *   1. Set the cluster-wide default read concern to {level: "majority"} via
 *      setDefaultRWConcern.
 *   2. Pause replication on the secondary with the stopReplProducer failpoint
 *      so subsequent writes cannot reach majority.
 *   3. Run a {w: "majority", wtimeout: ...} write that the primary applies
 *      locally but that times out waiting for replication. The write
 *      advances the primary's optime / cluster time.
 *   4. Start a causally-consistent session (which sends afterClusterTime
 *      automatically with NO explicit level on subsequent reads).
 *   5. Issue a read on the session.
 *
 * Expected post-fix behavior: the read inherits the cluster-wide default
 * read concern "majority" and blocks (returning ExceededTimeLimit when
 * capped via maxTimeMS) because the afterClusterTime is not yet
 * majority-committed.
 *
 * Pre-fix bug: the read silently resolves to read concern "local" and
 * returns the write that was never replicated, violating the operator's
 * setDefaultRWConcern contract.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = "test";
const collName = jsTestName();

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

// Seed the collection and replicate so the secondary has a baseline.
assert.commandWorked(primaryColl.insert({_id: 0, x: 1}, {writeConcern: {w: "majority"}}));

// Step 1: set the cluster-wide default read concern to "majority".
assert.commandWorked(
    primary.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "majority"},
    }),
);

// Step 2: pause replication on the secondary so subsequent writes cannot
// be majority-committed.
const stopReplProducer = configureFailPoint(secondary, "stopReplProducer");

// Step 3: issue a write that the primary applies locally but that cannot
// be majority-acknowledged. We expect WriteConcernFailed / wtimeout.
const writeRes = primaryDB.runCommand({
    update: collName,
    updates: [{q: {_id: 0}, u: {$inc: {x: 1}}}],
    writeConcern: {w: "majority", wtimeout: 2000},
});
assert.commandWorkedIgnoringWriteConcernErrors(writeRes);
assert(writeRes.writeConcernError,
       "Expected w:majority write to time out while replication is paused: " + tojson(writeRes));

// Step 4 + 5: start an explicit, causally-consistent session and issue a
// read. The driver attaches afterClusterTime automatically with no
// explicit level field. After the SERVER-126299 fix the server must
// resolve this read's level to the cluster-wide default ("majority").
const session = primary.startSession({causalConsistency: true});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

// First, perform a write inside the session so the session's clusterTime
// advances to a point that is NOT yet majority-committed. This mirrors
// the original Jira repro precisely.
assert.commandWorked(sessionColl.update({_id: 0}, {$inc: {x: 1}}, {writeConcern: {w: 1}}));

// A non-session read should still observe the primary's local state,
// because no afterClusterTime is sent. This anchors the "control" half
// of the experiment.
const noSessionDoc = primaryColl.findOne({_id: 0});
assert.neq(null,
           noSessionDoc,
           "Non-session read should always return the document on the primary.");

// A causally-consistent session read MUST block until the
// afterClusterTime entry is majority-committed. Replication is still
// paused, so the read should time out. Pre-fix, the read instead
// returned immediately with the local (uncommitted) document.
const readRes = sessionDB.runCommand({
    find: collName,
    filter: {_id: 0},
    maxTimeMS: 3000,
});

assert.commandFailedWithCode(
    readRes,
    ErrorCodes.MaxTimeMSExpired,
    "Pre-SERVER-126299 fix: the causally-consistent read silently used " +
        "readConcern:local and returned uncommitted data. After the fix it " +
        "should time out waiting for the afterClusterTime to reach majority.",
);

// Cleanup: lift the failpoint so the consistency checks at stopSet time
// can complete.
stopReplProducer.off();
session.endSession();
rst.stopSet();
