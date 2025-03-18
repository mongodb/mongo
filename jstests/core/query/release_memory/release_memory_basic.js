/**
 * Test basic case for releaseMemory command
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_preference_unchanged,
 *   assumes_superuser_permissions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_81,
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

function makeParallelShellFunctionString(cursorId, collName, sessionId, txnNumber) {
    let code = `const cursorId = ${cursorId};`;
    code += `const collName = "${collName}";`;
    TestData.sessionId = sessionId;
    TestData.txnNumber = txnNumber;

    const runGetMore = function() {
        let getMoreCmd = {
            getMore: cursorId,
            collection: collName,
            batchSize: 1,
            lsid: TestData.sessionId,
        };
        if (TestData.txnNumber != -1) {
            getMoreCmd.txnNumber = TestData.txnNumber;
        }

        assert.commandWorked(db.runCommand(getMoreCmd));
    };

    code += `(${runGetMore.toString()})();`;
    return code;
}

db.dropDatabase();
const coll = db[jsTestName()];
setupCollection(coll);

const createInactiveCursor = function(coll) {
    const cmdRes =
        coll.getDB().runCommand({find: coll.getName(), filter: {}, sort: {index: 1}, batchSize: 5});
    assert.commandWorked(cmdRes);
    return cmdRes.cursor.id;
};

const cursorToReleaseId = createInactiveCursor(coll);
const cursorNotFoundId = NumberLong("123");

const session = db.getMongo().startSession();
const cursorCurrentlyPinnedId =
    createInactiveCursor(session.getDatabase(db.getName())[jsTestName()]);
const getMoreFailpoint = configureFailPoint(db, "getMoreHangAfterPinCursor");

const joinGetMore =
    startParallelShell(makeParallelShellFunctionString(cursorCurrentlyPinnedId,
                                                       jsTestName(),
                                                       session.getSessionId(),
                                                       session.getTxnNumber_forTesting()));

getMoreFailpoint.wait();
waitForLog(20477, cursorCurrentlyPinnedId);

const releaseMemoryRes =
    db.runCommand({releaseMemory: [cursorToReleaseId, cursorNotFoundId, cursorCurrentlyPinnedId]});
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

const joinReleaseMemory = startParallelShell(funWithArgs(function(collName, cursorId) {
    assert.commandWorked(db.runCommand({releaseMemory: [cursorId]}));
}, jsTestName(), cursorIdForGetMore));

releaseMemoryFailPoint.wait();

assert.commandFailedWithCode(db.runCommand({getMore: cursorIdForGetMore, collection: jsTestName()}),
                             [ErrorCodes.CursorInUse]);

releaseMemoryFailPoint.off();
joinReleaseMemory();
