/**
 * Tests how internal writes are replicated when the systems collection is refreshed.
 *
 * @tags: [
 *  requires_replication,
 * ]
 */
(function() {
'use strict';

const kConfigSessionsNs = "config.system.sessions";

const refreshCmd = {
    refreshLogicalSessionCacheNow: 1
};

const startSessionCmd = {
    startSession: 1
};

// Refresh logical session cache and check that the number of sessions are as expected.
function refreshSessionsAndVerifyCount(config, expectedCount) {
    config.runCommand(refreshCmd);
    assert.eq(config.system.sessions.count(), expectedCount);
}

function findOplogOp(conn, operation, namespace) {
    return rst.findOplog(conn, {op: operation, ns: namespace}).toArray();
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

// Before refreshing logical session, expect that oplog is not updated with new inserts.
let ops = findOplogOp(primary, "i", kConfigSessionsNs);
assert.eq(0, ops.length);

// Refresh sessions to observe the insert of the implicit session.
refreshSessionsAndVerifyCount(configDB, 1);
ops = findOplogOp(primary, "i", kConfigSessionsNs);
jsTestLog("Implicit session inserted to sessions collection:\n" + tojson(ops));
assert.eq(1, ops.length);
const implicitSessionId = ops[0].o2._id;
const implicitSessionLastUse = ops[0].o.lastUse;

let sessionIDs = [];
(function insertAndRefreshSessions() {
    jsTestLog("Insert and refresh sessions");
    sessionIDs = createSessions(testDB);
    refreshSessionsAndVerifyCount(configDB, 6);

    ops = findOplogOp(primary, "i", kConfigSessionsNs);
    jsTestLog("New sessions inserted to sessions collection:\n" + tojson(ops));
    assert.eq(6, ops.length);

    ops = findOplogOp(primary, "u", kConfigSessionsNs);
    jsTestLog("Implicit session's lastUse time is updated:\n" + tojson(ops));
    assert.eq(1, ops.length);
    assert.eq(ops[0].o2._id, implicitSessionId);
    assert.neq(ops[0].o.diff.u.lastUse, implicitSessionLastUse);
})();

(function insertUpdateAndRefreshSessions() {
    jsTestLog("Insert new sessions and update existing sessions");
    createSessions(testDB);
    // Update lastUse of each new session we just created.
    let res = testDB.runCommand({refreshSessions: sessionIDs});
    assert.commandWorked(res, "unable to refresh session");
    refreshSessionsAndVerifyCount(configDB, 11);

    ops = findOplogOp(primary, "i", kConfigSessionsNs);
    jsTestLog("New sessions inserted to sessions collection:\n" + tojson(ops));
    assert.eq(11, ops.length);

    ops = findOplogOp(primary, "u", kConfigSessionsNs);
    jsTestLog("Implicit session's lastUse time is updated:\n" + tojson(ops));
    assert.eq(7, ops.length);
})();

rst.stopSet();
})();
