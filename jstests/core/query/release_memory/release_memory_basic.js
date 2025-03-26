/**
 * Test basic case for releaseMemory command
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_preference_unchanged,
 *   assumes_superuser_permissions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_82,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   uses_parallel_shell,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

function setupCollection(coll) {
    const docs = [];
    for (let i = 0; i < 200; ++i) {
        docs.push({_id: i, index: i});
    }
    assert.commandWorked(coll.insertMany(docs));
}

function waitForLog(logId, cursorId) {
    assert.soon(() => {
        const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
        if (findMatchingLogLine(globalLog.log, {id: logId, cursorId: cursorId})) {
            return true;
        }
        return false;
    });
}

function assertGetMore(cursorId, collName, sessionId, txnNumber) {
    let getMoreCmd = {
        getMore: cursorId,
        collection: collName,
        batchSize: 1,
        lsid: sessionId,
    };

    if (txnNumber != -1) {
        getMoreCmd.txnNumber = txnNumber;
    }
    jsTest.log.info("Running getMore in parallel shell: ", getMoreCmd);
    assert.commandWorked(db.runCommand(getMoreCmd));
}

db.dropDatabase();
const coll = db[jsTestName()];
setupCollection(coll);

const createInactiveCursor = function(coll) {
    const findCmd = {find: coll.getName(), filter: {}, sort: {index: 1}, batchSize: 5};
    jsTest.log.info("Running findCmd: ", findCmd);
    const cmdRes = coll.getDB().runCommand(findCmd);
    assert.commandWorked(cmdRes);
    return cmdRes.cursor.id;
};

const cursorToReleaseId = createInactiveCursor(coll);
const cursorNotFoundId = NumberLong("123");

const session = db.getMongo().startSession();
const cursorCurrentlyPinnedId =
    createInactiveCursor(session.getDatabase(db.getName())[jsTestName()]);
const getMoreFailpoint = configureFailPoint(db, "getMoreHangAfterPinCursor");

const joinGetMore = startParallelShell(funWithArgs(assertGetMore,
                                                   cursorCurrentlyPinnedId,
                                                   coll.getName(),
                                                   session.getSessionId(),
                                                   session.getTxnNumber_forTesting()));

getMoreFailpoint.wait();
waitForLog(20477, cursorCurrentlyPinnedId);

const releaseMemoryCmd = {
    releaseMemory: [cursorToReleaseId, cursorNotFoundId, cursorCurrentlyPinnedId]
};
jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
assert.commandWorked(releaseMemoryRes);

assert.eq(releaseMemoryRes.cursorsReleased, [cursorToReleaseId], releaseMemoryRes);
assert.eq(releaseMemoryRes.cursorsNotFound, [cursorNotFoundId], releaseMemoryRes);
assert.eq(releaseMemoryRes.cursorsCurrentlyPinned, [cursorCurrentlyPinnedId], releaseMemoryRes);

getMoreFailpoint.off();
joinGetMore();

const coll2 = db[jsTestName() + "2"];
setupCollection(coll2);

const cursorIdForGetMore = createInactiveCursor(coll);

const releaseMemoryFailPoint = configureFailPoint(db, "releaseMemoryHangAfterPinCursor");

const joinReleaseMemory = startParallelShell(funWithArgs(function(cursorId) {
    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory in parallel shell: ", releaseMemoryCmd);
    assert.commandWorked(db.runCommand(releaseMemoryCmd));
}, cursorIdForGetMore));

releaseMemoryFailPoint.wait();

assert.commandFailedWithCode(db.runCommand({getMore: cursorIdForGetMore, collection: jsTestName()}),
                             [ErrorCodes.CursorInUse]);

releaseMemoryFailPoint.off();
joinReleaseMemory();
