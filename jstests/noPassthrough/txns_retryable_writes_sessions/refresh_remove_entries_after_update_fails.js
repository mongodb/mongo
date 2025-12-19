/**
 * Test that when refreshing the timestamp in the sessions collection fails due to errors (like ShardNotFound or
 * WriteConcernErrors), the sessions pending to be ended are still processed.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

const conn = MongoRunner.runMongod();
const admin = conn.getDB("admin");
const config = conn.getDB("config");

const refreshCommand = {refreshLogicalSessionCacheNow: 1};
const startSessionCommand = {startSession: 1};

function getSessionCacheStats() {
    return admin.serverStatus().logicalSessionRecordCache;
}

jsTest.log.info("Initial refresh");
// Initial refresh to ensure clean start.
assert.commandWorked(admin.runCommand(refreshCommand), "failed initial refresh");

jsTest.log.info("Create 10 sessions");
let sessions = [];
for (let i = 0; i < 10; i++) {
    const res = admin.runCommand(startSessionCommand);
    assert.commandWorked(res, "unable to start session");
    sessions.push(res);
}

jsTest.log.info("Refresh after creating sessions");
assert.commandWorked(admin.runCommand(refreshCommand), "failed to refresh after creating sessions");
assert.eq(config.system.sessions.count(), 10, "should have 10 session records");
const statsAfterCreatingSessions = getSessionCacheStats();
jsTest.log.info("Session cache stats after creating 10 entries: " + tojson(statsAfterCreatingSessions));
assert.eq(statsAfterCreatingSessions.lastSessionsCollectionJobEntriesEnded, 0, "should not have ended any sessions");
assert.eq(
    statsAfterCreatingSessions.lastSessionsCollectionJobEntriesRefreshed,
    10,
    "Should have updated timestamps for 10 sessions created",
);

jsTest.log.info("End three sessions");

// End the first three sessions.
const endSessionsBatch1 = sessions.slice(0, 3).map((s) => s.id);
assert.commandWorked(admin.runCommand({endSessions: endSessionsBatch1}), "failed to end first batch of sessions");
jsTest.log.info("start one more session");
const eleventhSession = admin.runCommand(startSessionCommand);
assert.commandWorked(eleventhSession, "unable to start session");

// The failAllUpdates failpoint will cause the refreshSessions operation to fail
// since it uses update operations to refresh session lastUse timestamps.
jsTest.log.info("Enable failpoint that will call subsequent refreshes to fail when updating session timestamp");
const fp = configureFailPoint(conn, "failAllUpdates");

jsTest.log.info("Refresh after enabling failpoint");
// Attempt refresh - should fail due to the failpoint.
const refreshResult1 = admin.runCommand(refreshCommand);
assert.commandFailed(refreshResult1, "refresh should have failed with failAllUpdates enabled");

// The three sessions that were ended should have been removed from the collection.
// However, the newly added eleventh session should not have been added.
// Therefore, we expect there to be seven sessions in the collection.
assert.eq(
    config.system.sessions.count(),
    7,
    "expect 7 sessions = 10 original sessions - 3 ended, without the 11th one added",
);
const statsAfterFirstBatch = getSessionCacheStats();
jsTest.log.info("Session cache stats after first batch ended: " + tojson(statsAfterFirstBatch));
assert.eq(
    statsAfterFirstBatch.lastSessionsCollectionJobEntriesEnded,
    3,
    "should have ended 3 sessions from the last batch",
);
assert.eq(
    statsAfterFirstBatch.lastSessionsCollectionJobEntriesRefreshedUpdatedTimestamp,
    statsAfterCreatingSessions.lastSessionsCollectionJobEntriesRefreshedUpdatedTimestamp,
    "should not have been able to refresh eleventh session due to failpoint",
);

// End more sessions (sessions 4-6) while the failpoint is still active.
jsTest.log.info("End three more sessions");
const endSessionsBatch2 = sessions.slice(3, 6).map((s) => s.id);
assert.commandWorked(admin.runCommand({endSessions: endSessionsBatch2}), "failed to end second batch of sessions");

// Attempt refresh again - should still fail.
jsTest.log.info("Attempt to refresh again - failpoint is still active");
const refreshResult2 = admin.runCommand(refreshCommand);
assert.commandFailed(refreshResult2, "refresh should still fail with failAllUpdates enabled");

const statsAfterSecondBatch = getSessionCacheStats();
jsTest.log.info("Session cache stats after second batch ended: " + tojson(statsAfterSecondBatch));
assert.eq(
    statsAfterSecondBatch.lastSessionsCollectionJobEntriesEnded,
    3,
    "should have ended 3 sessions from the last batch",
);
assert.eq(
    statsAfterSecondBatch.lastSessionsCollectionJobEntriesRefreshedUpdatedTimestamp,
    statsAfterCreatingSessions.lastSessionsCollectionJobEntriesRefreshedUpdatedTimestamp,
    "should not have been able to refresh eleventh session due to failpoint",
);
// Six sessions have now been ended, and we are still missing the 11th session due to the failpoint.
assert.eq(
    config.system.sessions.count(),
    4,
    "expect 4 sessions = 10 original sessions - 3 ended in first batch - 3 ended in second batch, without the 11th one added",
);

// Disable the failpoint.
fp.off();

jsTest.log.info("End two more sessions after disabling failpoint");
// End more sessions (sessions 7-8) without the failpoint enabled.
const endSessionsBatch3 = sessions.slice(6, 8).map((s) => s.id);
assert.commandWorked(admin.runCommand({endSessions: endSessionsBatch3}), "failed to end third batch of sessions");

// Now refresh should succeed, and the eleventh session should be added to the collection.
assert.commandWorked(admin.runCommand(refreshCommand), "refresh should succeed after disabling failpoint");
assert.eq(
    config.system.sessions.count(),
    3,
    "expect 3 sessions = 10 original sessions - 3 ended in first batch - 3 ended in second batch - 2 ended in third batch + the 11th one",
);

// Get stats after successful refresh.
const statsAfter = getSessionCacheStats();
jsTest.log.info("Session cache stats after successful refresh: " + tojson(statsAfter));
assert.eq(statsAfter.lastSessionsCollectionJobEntriesEnded, 2, "should have ended at least 8 accumulated sessions");
assert.eq(statsAfter.lastSessionsCollectionJobEntriesRefreshed, 1, "should have been able to refresh eleventh session");

// Cleanup: end remaining sessions.
const remainingSessions = sessions.slice(8, 10).map((s) => s.id);
assert.commandWorked(admin.runCommand({endSessions: remainingSessions}));
const twelfthSession = admin.runCommand(startSessionCommand);
assert.commandWorked(twelfthSession, "unable to start session");
assert.commandWorked(admin.runCommand(refreshCommand));
assert.eq(config.system.sessions.count(), 2, "original 10 sessions should be cleaned up");
assert.commandWorked(admin.runCommand({endSessions: [eleventhSession.id]}));
assert.commandWorked(admin.runCommand({endSessions: [twelfthSession.id]}));
assert.commandWorked(admin.runCommand(refreshCommand));
assert.eq(config.system.sessions.count(), 0, "all sessions should be cleaned up");

MongoRunner.stopMongod(conn);
