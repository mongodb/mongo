/**
 * Test to surface issues with the semantics of the refreshLogicalSessionCacheNow command.
 * Using a replset with a short logical session refresh interval, run the command several times to make sure that sessions are inserted when it returns.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const numIterations = 10;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set short interval on logical session refresh
            logicalSessionRefreshMillis: 100,
            disableLogicalSessionCacheRefresh: false,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");
const sessionsColl = primary.getDB("config").system.sessions;

// Basic sanity check that the sessions collection will be refreshed with a new session without calling
// refreshLogicalSessionCacheNow, since the logical session refresh interval is short.
{
    const session = primary.startSession();
    const sessionId = session.getSessionId();
    const sessionDB = session.getDatabase("test");
    assert.commandWorked(sessionDB.runCommand({insert: "coll", documents: [{_id: 1000}]}));

    jsTestLog.info(
        `Waiting for session ${tojson(sessionId)} to be added to the cache after logicalSessionRefreshMillis elapses`,
    );
    assert.soon(() => {
        return sessionsColl.find({"_id.id": sessionId.id}).itcount() === 1;
    }, "Session should be present after logicalSessionRefreshMillis elapses");

    session.endSession();
}

const sessions = [];
for (let i = 0; i < numIterations; i++) {
    jsTestLog.info(`Starting session ${i} and using it to add to the cache`);
    const session = primary.startSession();
    sessions.push(session);
    const sessionId = session.getSessionId();
    const sessionDB = session.getDatabase("test");
    assert.commandWorked(sessionDB.runCommand({insert: "coll", documents: [{_id: i}]}));

    jsTestLog.info(`Call refreshLogicalSessionCacheNow iteration ${i}`);
    assert.commandWorked(adminDB.runCommand({refreshLogicalSessionCacheNow: 1}));
    assert.eq(
        1,
        sessionsColl.find({"_id.id": sessionId.id}).itcount(),
        "Session should be present after refreshLogicalSessionCacheNow finishes",
    );
}
for (const session of sessions) {
    session.endSession();
}
rst.stopSet();
