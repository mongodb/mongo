// SERVER-7200 On startup, replica set nodes delete oplog state past the oplog delete point and
// apply any remaining unapplied ops before coming up as a secondary. If the op fails to apply, the
// server must fail to start up.
//
// @tags: [
//   requires_persistence,
//   incompatible_with_windows_tls,
// ]
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Because this test intentionally causes the server to crash, we need to instruct the
// shell to clean up the core dump that is left behind.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

let rst = new ReplSetTest({
    nodes: 1,
});

rst.startSet();
rst.initiate();

let conn = rst.getPrimary(); // Waits for PRIMARY state.

// Wait for the commit point to reach the top of the oplog so that the stableTS can advance.
assert.soon(function () {
    const optimes = assert.commandWorked(conn.adminCommand({replSetGetStatus: 1})).optimes;
    return 0 == rs.compareOpTimes(optimes.lastCommittedOpTime, optimes.appliedOpTime);
});

conn = rst.restart(0, {noReplSet: true}); // Restart as a standalone node.
assert.neq(null, conn, "failed to restart");

let oplog = conn.getCollection("local.oplog.rs");
let lastOplogDoc = conn.getCollection("local.oplog.rs").find().sort({$natural: -1}).limit(1)[0];
let lastTs = lastOplogDoc.ts;
let newTs = Timestamp(lastTs.t + 1, 1);
let term = lastOplogDoc.t;

assert.commandWorked(
    oplog.insert({
        ts: newTs,
        t: term,
        h: 1,
        op: "c",
        ns: "somedb.$cmd",
        o: {thereIsNoCommandWithThisName: 1},
    }),
);

let injectedMinValidDoc = {
    _id: ObjectId(),

    // appliedThrough
    begin: {
        ts: lastTs,
        t: term,
    },

    // minvalid:
    t: term,
    ts: newTs,
};

// This weird mechanism is the only way to bypass mongod's attempt to fill in null
// Timestamps.
let minValidColl = conn.getCollection("local.replset.minvalid");
assert.commandWorked(minValidColl.remove({}));
assert.commandWorked(minValidColl.update({}, {$set: injectedMinValidDoc}, {upsert: true}));
assert.eq(
    minValidColl.findOne(),
    injectedMinValidDoc,
    "If the Timestamps differ, the server may be filling in the null timestamps",
);

assert.throws(() => rst.restart(0)); // Restart in replSet mode again.

rst.stop(0, undefined, {allowedExitCode: MongoRunner.EXIT_ABORT});
rst.stopSet();
