/**
 * Tests that getMore will retry pinning the cursor if releaseMemory holds the pin.
 * @tags: [
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
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
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

const failpointName = FixtureHelpers.isMongos(db) ? "clusterReleaseMemoryHangAfterPinCursor"
                                                  : "releaseMemoryHangAfterPinCursor";

const getMoreRetriesKnob = "internalQueryGetMoreMaxCursorPinRetryAttempts";
const originalKnobValue = getServerParameter(getMoreRetriesKnob);

{
    const cursorIdForGetMore = createInactiveCursor(coll);

    const releaseMemoryFailPoint = configureFailPoint(db, failpointName);

    const joinReleaseMemory = startParallelShell(funWithArgs(function(cursorId) {
        const releaseMemoryCmd = {releaseMemory: [cursorId]};
        jsTest.log.info("Running releaseMemory in parallel shell: ", releaseMemoryCmd);
        assert.commandWorked(db.runCommand(releaseMemoryCmd));
    }, cursorIdForGetMore));

    releaseMemoryFailPoint.wait();

    // getMore retries pinning the cursor if releaseMemory holds the pin, but it does it for a
    // limited amount of retries.
    setServerParameter(getMoreRetriesKnob, 2);
    assert.commandFailedWithCode(
        db.runCommand({getMore: cursorIdForGetMore, collection: jsTestName()}),
        [ErrorCodes.CursorInUse]);

    releaseMemoryFailPoint.off();
    joinReleaseMemory();
}

const retryLogId = FixtureHelpers.isMongos(db) ? 10546601 : 10116501;

{  // Run getMore and releaseMemory a lot in parallel to ensure that getMore can retry pinning the
   // cursor.
    // Set the knob to a very high value to ensure practically infinite retries.
    setServerParameter(getMoreRetriesKnob, 1000000000);

    db.setLogLevel(3, "query");

    const session = db.getMongo().startSession();
    const cursorId = createInactiveCursor(session.getDatabase(db.getName())[jsTestName()]);

    const releaseMemoryFailPoint = configureFailPoint(db, failpointName);

    const joinReleaseMemory = startParallelShell(funWithArgs(function(cursorId) {
        const releaseMemoryCmd = {releaseMemory: [cursorId]};
        jsTest.log.info("Running releaseMemory in parallel shell: ", releaseMemoryCmd);
        for (let i = 0; i < 200; ++i) {
            const res = db.runCommand(releaseMemoryCmd);
            assert.commandWorked(res);
            if (res.cursorsReleased.length > 0) {
                assert.eq(res.cursorsReleased, [cursorId], res);
            } else {
                assert.eq(res.cursorsCurrentlyPinned, [cursorId], res);
            }
        }
    }, cursorId));

    releaseMemoryFailPoint.wait();

    const joinGetMore = startParallelShell(funWithArgs(assertGetMore,
                                                       cursorId,
                                                       coll.getName(),
                                                       session.getSessionId(),
                                                       session.getTxnNumber_forTesting(),
                                                       150 /*times*/));

    // Assert that at least one retry happened.
    waitForLog({id: retryLogId});
    releaseMemoryFailPoint.off();

    joinReleaseMemory();
    joinGetMore();
}

setServerParameter(getMoreRetriesKnob, originalKnobValue);
