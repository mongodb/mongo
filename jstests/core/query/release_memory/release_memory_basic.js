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
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

function setupCollection(coll) {
    const docs = [];
    for (let i = 0; i < 200; ++i) {
        docs.push({_id: i, index: i});
    }
    assert.commandWorked(coll.insertMany(docs));
}

function waitForLog(predicate) {
    assert.soon(() => {
        const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
        if (findMatchingLogLine(globalLog.log, predicate)) {
            return true;
        }
        return false;
    });
}

function assertGetMore(cursorId, collName, sessionId, txnNumber, times) {
    let getMoreCmd = {
        getMore: cursorId,
        collection: collName,
        batchSize: 1,
        lsid: sessionId,
    };

    if (txnNumber != -1) {
        getMoreCmd.txnNumber = txnNumber;
    }
    for (let i = 0; i < times; ++i) {
        jsTest.log.info("Running getMore in parallel shell: ", getMoreCmd);
        assert.commandWorked(db.runCommand(getMoreCmd));
    }
}

db.dropDatabase();
const coll = db[jsTestName()];
setupCollection(coll);

const createInactiveCursor = function(coll) {
    const findCmd = {find: coll.getName(), filter: {}, sort: {index: 1}, batchSize: 1};
    jsTest.log.info("Running findCmd: ", findCmd);
    const cmdRes = coll.getDB().runCommand(findCmd);
    assert.commandWorked(cmdRes);
    return cmdRes.cursor.id;
};

{
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
                                                       session.getTxnNumber_forTesting(),
                                                       1 /*times*/));

    getMoreFailpoint.wait();
    waitForLog({id: 20477, cursorId: cursorCurrentlyPinnedId});

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
}
