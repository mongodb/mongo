/**
 * Regression test for SERVER-126311: a candidate that has received oplog
 * entries through the log stream but has not yet finished applying them must
 * not bypass the step-up catchup phase.
 *
 * The bug being guarded: a new primary that bypassed catchup wrote a fresh
 * 'create config.transactions' oplog entry with a new UUID, dropping the
 * previously majority-committed writes that other nodes still held. The
 * downstream failure was an invariant crash in
 * createCollectionForApplyOps / renameOutOfTheWayForApplyOps on secondaries
 * in steady-state apply mode (BF-43238). This test exercises the
 * pre-step-up window by stopping replication on the secondary that we
 * will later step up, driving the old primary forward, then forcing
 * step-up on the lagging node. The fix ensures the new primary still has
 * the prior writes in its oplog after step-up completes.
 *
 * @tags: [
 *   requires_mongobridge,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {getLatestOp, reconnect} from "jstests/replsets/rslib.js";

const name = "stepup_catchup_no_bypass_with_oplog";
const rst = new ReplSetTest({
    name: name,
    nodes: 3,
    useBridge: true,
    waitForKeys: true,
});

rst.startSet();
const conf = rst.getReplSetConfig();
// Node 2 is a permanent secondary; we will step up node 1.
conf.members[2].priority = 0;
conf.settings = {
    heartbeatIntervalMillis: 500,
    electionTimeoutMillis: 10000,
    catchUpTimeoutMillis: 4 * 60 * 1000,
};
rst.initiate(conf);
rst.awaitSecondaryNodes();

let primary = rst.getPrimary();
const secondaries = rst.getSecondaries();
const candidate = secondaries[0];
const voter = secondaries[1];
const collName = "stepup_catchup_no_bypass_with_oplog_coll";
const primaryColl = primary.getDB("test")[collName];

// Bring down default WC to 1 so we can simulate writes that have not yet been
// majority-replicated to the candidate when step-up happens.
assert.commandWorked(
    primary.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1},
        writeConcern: {w: "majority"},
    }),
);

// Seed both secondaries with one majority-committed write so the catalog is
// established on every node before we begin freezing replication. This is
// the baseline state every node agrees on.
assert.commandWorked(primaryColl.insert({_id: 0, seed: true}, {writeConcern: {w: "majority"}}));
rst.awaitReplication();

jsTestLog("Freezing replication on the candidate and generating oplog ops on the old primary.");

// Halt replication on the candidate so the old primary's subsequent writes do
// not reach it. The voter keeps replicating normally so it can vote yes during
// the step-up.
stopServerReplication(candidate);

// Old primary writes a known batch of ops. These must survive the step-up.
const newOpCount = 4;
for (let i = 1; i <= newOpCount; i++) {
    assert.commandWorked(primaryColl.insert({_id: i, value: "pre_stepup_" + i}, {writeConcern: {w: 1}}));
}

const latestOpOnOldPrimary = getLatestOp(primary);
jsTestLog("Latest op on old primary before step-up: " + tojson(latestOpOnOldPrimary));

// Snapshot oplog state on the voter (which has replicated all of the new ops)
// so we can later confirm the new primary's oplog matches.
const voterOplog = voter.getDB("local").oplog.rs;
const voterOpCount = voterOplog.find({ns: primaryColl.getFullName(), op: "i"}).itcount();
jsTestLog("Voter sees " + voterOpCount + " inserts on " + primaryColl.getFullName() + " before step-up.");

// Now resume replication on the candidate JUST long enough for the log stream
// to push the new ops to its oplog. We will NOT let it apply / checkpoint
// them. This mirrors the "lastWrittenOplogLSN > 0 but no checkpoint installed
// yet" pre-condition that the bug exploited.
restartServerReplication(candidate);

assert.soon(
    () => {
        const candidateOplogCount = candidate
            .getDB("local")
            .oplog.rs.find({ns: primaryColl.getFullName(), op: "i"})
            .itcount();
        return candidateOplogCount >= voterOpCount;
    },
    "Candidate did not receive all oplog inserts before step-up timeout",
    60 * 1000,
);
const candidateOplogCountPreStep = candidate
    .getDB("local")
    .oplog.rs.find({ns: primaryColl.getFullName(), op: "i"})
    .itcount();
jsTestLog("Candidate carries " + candidateOplogCountPreStep + " oplog inserts before step-up.");

// Force step-up on the lagging candidate. With the fix in place, the catchup
// gate refuses the bypass when the oplog is non-empty and no checkpoint is
// installed; the candidate may either complete catchup normally or step
// back down. Either way the oplog must not have lost any of the prior ops.
jsTestLog("Stepping up the candidate.");
assert.soonNoExcept(
    function () {
        return candidate.adminCommand({replSetStepUp: 1}).ok;
    },
    "Candidate did not accept replSetStepUp",
    60 * 1000,
);

// Wait for the new primary to settle. Either the candidate became primary or
// it stepped back down and the original primary re-took the role; either way
// rst.getPrimary() converges.
rst.nodes.forEach(reconnect);
const newPrimary = rst.getPrimary();
rst.awaitReplication();

jsTestLog("Settled primary post-step-up: " + newPrimary.host);

// Core safety assertion for SERVER-126311: every previously-written op on the
// old primary must still be present in the new primary's oplog. If the bypass
// had been taken, the new primary would have written a fresh catalog and the
// old inserts would not appear in its oplog at all.
const newPrimaryOplog = newPrimary.getDB("local").oplog.rs;
const newPrimaryOpCount = newPrimaryOplog.find({ns: primaryColl.getFullName(), op: "i"}).itcount();
assert.eq(
    voterOpCount,
    newPrimaryOpCount,
    "Oplog write count mismatch after step-up: voter had " +
        voterOpCount +
        " inserts on " +
        primaryColl.getFullName() +
        " but new primary " +
        newPrimary.host +
        " has " +
        newPrimaryOpCount +
        ". This indicates the step-up catchup bypass dropped writes.",
);

// And the documents themselves must be readable on the new primary.
const docCount = newPrimary.getDB("test")[collName].find().itcount();
assert.eq(
    newOpCount + 1,
    docCount,
    "Document count on new primary does not match expected post-step-up state: " +
        docCount +
        " vs expected " +
        (newOpCount + 1),
);

rst.stopSet();
