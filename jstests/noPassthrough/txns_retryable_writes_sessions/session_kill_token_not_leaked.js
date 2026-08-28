/**
 * A session kill token which is dropped instead of being consumed must still return its kill.
 * Otherwise 'SessionRuntimeInfo::killsRequested' never returns to zero and every later check-out of
 * that session family blocks forever, because the normal check-out path has no deadline.
 *
 * The token is dropped here the way production did it: 'killSessionsAction' holds the token for the
 * session it is about to kill while it waits for the kill check-out, so interrupting that wait
 * destroys the token without the kill ever having been returned.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// 'killAllSessions' must not run under a session of its own.
TestData.disableImplicitSessions = true;

// Only reached if the kill was leaked, in which case the check-out blocks forever.
const kCheckOutTimeoutMS = 5 * 60 * 1000;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

function outstandingKills(conn) {
    return assert.commandWorked(conn.adminCommand({serverStatus: 1})).logicalSessionRecordCache
        .sessionsWithOutstandingKills;
}

const adminDB = primary.getDB("admin");
const dbName = jsTestName();
const db = primary.getDB(dbName);
const lsid = {id: UUID()};

assert.commandWorked(db.createCollection("c"));

// Only sessions with a transaction in progress are selected by the kill below, so leave one open.
assert.commandWorked(
    db.runCommand({
        insert: "c",
        documents: [{_id: 0}],
        lsid: lsid,
        txnNumber: NumberLong(1),
        startTransaction: true,
        autocommit: false,
    }),
);

// Hang the kill check-out, so the token is held but not yet consumed when it is interrupted.
const fp = configureFailPoint(primary, "hangAfterIncrementingNumWaitingToCheckOut");

// The command hands the work to the SessionKiller thread, which is what ends up holding the token.
const killer = startParallelShell(() => {
    // Expected to fail, because the SessionKiller is interrupted below.
    db.getSiblingDB("admin").runCommand({killAllSessions: []});
}, primary.port);

fp.wait();

// Interrupt the SessionKiller while it holds the token. It is a background thread, so it is only
// reported with 'idleConnections'.
const allOps = adminDB
    .aggregate([{$currentOp: {allUsers: true, idleConnections: true, localOps: true}}])
    .toArray();
const killerOps = allOps.filter((op) => op.desc === "SessionKiller" && op.opid !== undefined);
assert.eq(1, killerOps.length, () => tojson(allOps));
assert.commandWorked(adminDB.runCommand({killOp: 1, op: killerOps[0].opid}));

fp.off();
killer();

// Nothing holds a kill token any more, so no kill may be outstanding.
assert.soon(
    () => outstandingKills(primary) === 0,
    () =>
        `a session kill token was leaked: ${outstandingKills(primary)} session(s) still ` +
        `have an outstanding kill`,
    30 * 1000,
);

// And the session must be usable again.
assert.commandWorked(
    db.runCommand({
        insert: "c",
        documents: [{_id: 1}],
        lsid: lsid,
        txnNumber: NumberLong(2),
        maxTimeMS: kCheckOutTimeoutMS,
    }),
);

// The accounting is back to zero, so the session is still killable.
assert.commandWorked(adminDB.runCommand({killAllSessions: []}));
assert.commandWorked(
    db.runCommand({
        insert: "c",
        documents: [{_id: 2}],
        lsid: lsid,
        txnNumber: NumberLong(3),
        maxTimeMS: kCheckOutTimeoutMS,
    }),
);

rst.stopSet();
