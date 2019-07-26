// Tests valid coordination of the expiration and vivification of documents between the
// config.system.sessions collection and the logical session cache.
//
// 1. Sessions should be removed from the logical session cache when they expire from
//    the config.system.sessions collection.
// 2. getMores run on open cursors should update the lastUse field on documents in the
//    config.system.sessions collection, prolonging the time for expiration on said document
//    and corresponding session.
// 3. Open cursors that are not currently in use should be killed when their corresponding sessions
//    expire from the config.system.sessions collection.
// 4. Currently running operations corresponding to a session should prevent said session from
//    expiring from the config.system.sessions collection. If the expiration date has been reached
//    during a currently running operation, the logical session cache should vivify the session and
//    replace it in the config.system.sessions collection.

(function() {
"use strict";

// This test makes assertions about the number of logical session records.
TestData.disableImplicitSessions = true;

load("jstests/libs/pin_getmore_cursor.js");  // For "withPinnedCursor".

const refresh = {
    refreshLogicalSessionCacheNow: 1
};
const startSession = {
    startSession: 1
};
const failPointName = "waitAfterPinningCursorBeforeGetMoreBatch";

function refreshSessionsAndVerifyCount(config, expectedCount) {
    config.runCommand(refresh);
    assert.eq(config.system.sessions.count(), expectedCount);
}

function getSessions(config) {
    return config.system.sessions.aggregate([{'$listSessions': {allUsers: true}}]).toArray();
}

const dbName = "test";
const testCollName = "verify_sessions_find_get_more";

let replTest = new ReplSetTest({name: 'refresh', nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
replTest.awaitSecondaryNodes();

let db = primary.getDB(dbName);
let config = primary.getDB("config");

// 1. Verify that sessions expire from config.system.sessions after the timeout has passed.
for (let i = 0; i < 5; i++) {
    let res = db.runCommand(startSession);
    assert.commandWorked(res, "unable to start session");
}
refreshSessionsAndVerifyCount(config, 5);

// Manually delete entries in config.system.sessions to simulate TTL expiration.
assert.commandWorked(config.system.sessions.remove({}));
refreshSessionsAndVerifyCount(config, 0);

// 2. Verify that getMores after finds will update the 'lastUse' field on documents in the
// config.system.sessions collection.
for (let i = 0; i < 10; i++) {
    db[testCollName].insert({_id: i, a: i, b: 1});
}

let cursors = [];
for (let i = 0; i < 5; i++) {
    let session = db.getMongo().startSession({});
    assert.commandWorked(session.getDatabase("admin").runCommand({usersInfo: 1}),
                         "initialize the session");
    cursors.push(session.getDatabase(dbName)[testCollName].find({b: 1}).batchSize(1));
    assert(cursors[i].hasNext());
}

refreshSessionsAndVerifyCount(config, 5);

let sessionsCollectionArray;
let lastUseValues = [];
for (let i = 0; i < 3; i++) {
    for (let j = 0; j < cursors.length; j++) {
        cursors[j].next();
    }

    refreshSessionsAndVerifyCount(config, 5);

    sessionsCollectionArray = getSessions(config);

    if (i == 0) {
        for (let j = 0; j < sessionsCollectionArray.length; j++) {
            lastUseValues.push(sessionsCollectionArray[j].lastUse);
        }
    } else {
        for (let j = 0; j < sessionsCollectionArray.length; j++) {
            assert.gt(sessionsCollectionArray[j].lastUse, lastUseValues[j]);
            lastUseValues[j] = sessionsCollectionArray[j].lastUse;
        }
    }
}

// 3. Verify that letting sessions expire (simulated by manual deletion) will kill their
// cursors.
assert.commandWorked(config.system.sessions.remove({}));
refreshSessionsAndVerifyCount(config, 0);

for (let i = 0; i < cursors.length; i++) {
    assert.commandFailedWithCode(
        db.runCommand({getMore: cursors[i]._cursor._cursorid, collection: testCollName}),
        ErrorCodes.CursorNotFound,
        'expected getMore to fail because the cursor was killed');
}

// 4. Verify that an expired session (simulated by manual deletion) that has a currently running
// operation will be vivified during the logical session cache refresh.
let pinnedCursorSession = db.getMongo().startSession();
let pinnedCursorDB = pinnedCursorSession.getDatabase(dbName);

withPinnedCursor({
    conn: primary,
    db: pinnedCursorDB,
    assertFunction: (cursorId, coll) => {
        assert.commandWorked(config.system.sessions.remove({}));
        refreshSessionsAndVerifyCount(config, 1);

        let db = coll.getDB();
        assert.commandWorked(db.runCommand({killCursors: coll.getName(), cursors: [cursorId]}));
    },
    runGetMoreFunc: () => {
        db.runCommand({getMore: cursorId, collection: collName});
    },
    failPointName: failPointName
},
                 /* assertEndCounts */ false);

replTest.stopSet();
})();
