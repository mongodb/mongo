/**
 * Test that releaseMemory correctly interacts with transactions
 * @tags: [
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 *   assumes_read_preference_unchanged,
 *   assumes_superuser_permissions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_82,
 *   uses_transactions,
 * ]
 */

import {assertReleaseMemoryWorked} from "jstests/libs/release_memory_util.js";

function setupCollection(coll) {
    const docs = [];
    for (let i = 0; i < 200; ++i) {
        docs.push({_id: i, index: i});
    }
    assert.commandWorked(coll.insertMany(docs));
}

function createInactiveCursor(coll) {
    const findCmd = {find: coll.getName(), filter: {}, sort: {index: 1}, batchSize: 1};
    const cmdRes = coll.getDB().runCommand(findCmd);
    assert.commandWorked(cmdRes);
    return cmdRes.cursor.id;
};

db.dropDatabase();
const coll = db[jsTestName()];
setupCollection(coll);

const outOfSessionCursorId = createInactiveCursor(coll);

const session = db.getMongo().startSession();
const sessionDb = session.getDatabase(db.getName());
const sessionColl = sessionDb[jsTestName()];

try {
    session.startTransaction();
    const sessionCursorId = createInactiveCursor(sessionColl);

    {
        const sessionReleaseMemoryRes =
            sessionDb.runCommand({releaseMemory: [sessionCursorId, outOfSessionCursorId]});
        assert.commandWorked(sessionReleaseMemoryRes);
        assertReleaseMemoryWorked(sessionReleaseMemoryRes, sessionCursorId);
        assertReleaseMemoryWorked(sessionReleaseMemoryRes, outOfSessionCursorId);
    }

    {
        const outOfSessionReleaseMemoryRes =
            db.runCommand({releaseMemory: [sessionCursorId, outOfSessionCursorId]});
        assert.commandWorked(outOfSessionReleaseMemoryRes);
        assertReleaseMemoryWorked(outOfSessionReleaseMemoryRes, sessionCursorId);
        assertReleaseMemoryWorked(outOfSessionReleaseMemoryRes, outOfSessionCursorId);
    }
} finally {
    session.abortTransaction();
}
