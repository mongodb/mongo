/**
 * Tests how internal writes are replicated when the systems collection is refreshed.
 *
 * @tags: [
 *  requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kConfigSessionsNs = "config.system.sessions";

const refreshCmd = {
    refreshLogicalSessionCacheNow: 1,
};

const startSessionCmd = {
    startSession: 1,
};

function findOplogOp(conn, operation, namespace) {
    return rst.findOplog(conn, {op: operation, ns: namespace}).toArray();
}

function countOplogOps(conn, operation, namespace) {
    return findOplogOp(conn, operation, namespace).length;
}

function createSessions(db) {
    let sessionIDs = [];
    for (let i = 0; i < 5; i++) {
        let res = db.runCommand(startSessionCmd);
        assert.commandWorked(res, "unable to start session");
        sessionIDs.push({id: res.id.id});
    }
    return sessionIDs;
}

const dbName = "test";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

let testDB = primary.getDB(dbName);
let configDB = primary.getDB("config");

// Phase 1 - baseline: no insert oplog entries exist yet for config.system.sessions because the
// logical session cache has not been flushed.
assert.eq(0, countOplogOps(primary, "i", kConfigSessionsNs));

// Phase 2 - first refresh: flushing the cache persists every session that has been registered up
// to this point. ReplSetTest's setup helpers (startSet, initiate, getPrimary, ...) may open
// multiple connections to the primary, each carrying its own implicit lsid, so the exact number
// of pre-existing sessions is not stable across versions. Snapshot the post-refresh state and
// assert deltas from it for the rest of the test.
assert.commandWorked(configDB.runCommand(refreshCmd));
const baselineCount = configDB.system.sessions.count();
assert.gte(baselineCount, 1);
assert.eq(baselineCount, countOplogOps(primary, "i", kConfigSessionsNs));

// Track the shell's implicit session for `configDB` -- it is guaranteed to be one of the
// baseline sessions and gets its lastUse bumped on each subsequent refresh. Match on the lsid
// `id` (UUID) field; the on-disk `_id` is the full lsid object (including a `uid` derived from
// the authenticated user) so a direct equality check on the whole object is too strict.
const implicitSessionUUID = configDB.getSession().getSessionId().id;
const implicitInsertOp = findOplogOp(primary, "i", kConfigSessionsNs).find((op) => {
    const lsid = op.o2?._id ?? op.o?._id;
    return lsid && bsonWoCompare({u: lsid.id}, {u: implicitSessionUUID}) === 0;
});

assert(implicitInsertOp, "configDB's implicit session was not flushed to config.system.sessions");
const implicitSessionLastUse = implicitInsertOp.o.lastUse;

let sessionIDs = [];
// Phase 3 - insertAndRefreshSessions: create 5 new sessions, then refresh. Expect baseline+5
// total docs / inserts, plus exactly 1 update entry: the implicit session's lastUse bump.
(function insertAndRefreshSessions() {
    jsTestLog("Insert and refresh sessions");
    sessionIDs = createSessions(testDB);
    assert.commandWorked(configDB.runCommand(refreshCmd));
    assert.eq(baselineCount + 5, configDB.system.sessions.count());

    let ops = findOplogOp(primary, "i", kConfigSessionsNs);
    jsTestLog("New sessions inserted to sessions collection:\n" + tojson(ops));
    assert.eq(baselineCount + 5, ops.length);

    ops = findOplogOp(primary, "u", kConfigSessionsNs);
    jsTestLog("Implicit session's lastUse time is updated:\n" + tojson(ops));
    assert.eq(1, ops.length);
    assert.eq(
        ops[0].o2._id,
        implicitInsertOp.o2 && implicitInsertOp.o2._id
            ? implicitInsertOp.o2._id
            : implicitInsertOp.o._id,
    );
    assert.neq(ops[0].o.diff.u.lastUse, implicitSessionLastUse);
})();

// Phase 4 - insertUpdateAndRefreshSessions: create 5 more sessions and explicitly refresh the
// original 5 from phase 3 to bump their lastUse. Expect baseline+10 total docs / inserts and 7
// total update entries (1 from phase 3 + 5 from the explicit refreshSessions + 1 more for the
// implicit session being bumped again).
(function insertUpdateAndRefreshSessions() {
    jsTestLog("Insert new sessions and update existing sessions");
    createSessions(testDB);
    // Update lastUse of each new session we just created.
    let res = testDB.runCommand({refreshSessions: sessionIDs});
    assert.commandWorked(res, "unable to refresh session");
    assert.commandWorked(configDB.runCommand(refreshCmd));
    assert.eq(baselineCount + 10, configDB.system.sessions.count());

    let ops = findOplogOp(primary, "i", kConfigSessionsNs);
    jsTestLog("New sessions inserted to sessions collection:\n" + tojson(ops));
    assert.eq(baselineCount + 10, ops.length);

    ops = findOplogOp(primary, "u", kConfigSessionsNs);
    jsTestLog("Implicit session's lastUse time is updated:\n" + tojson(ops));
    assert.eq(7, ops.length);
})();

// Teardown.
rst.stopSet();
