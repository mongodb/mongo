/**
 * Test that the ingress admission control works correctly.
 * @tags: [requires_fcv_80]
 */

import {waitForCurOpByComment} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

/**
 * Test that we are not allowed to set the pool size to a negative value.
 */
function testPoolSizeValidation(db) {
    assert.commandFailedWithCode(
        db.adminCommand({setParameter: 1, ingressAdmissionControllerTicketPoolSize: -1}),
        ErrorCodes.BadValue);
}

/**
 * Test that the operations waiting for ingress admission can be distinguished in currentOp.
 */
function testCurrentOp(conn, db, collName) {
    const kComment = "ingress admission queue curop test";

    // run a parallel shell that can wait for admission control
    const parallelShell = startParallelShell(
        funWithArgs((dbName, collName, comment) => {
            let testDB = db.getSiblingDB(dbName);

            // make the next controlled command wait for ingress admission
            assert.commandWorked(testDB.adminCommand(
                {setParameter: 1, ingressAdmissionControllerTicketPoolSize: 0}));

            // wait until the command has been admitted
            assert.commandWorked(testDB.runCommand({count: collName, comment: comment}));
        }, db.getName(), collName, kComment), conn.port, false);

    // confirm that our operation is waiting for ingress admission
    const opsBeforeAdmission =
        waitForCurOpByComment(db, kComment, {"currentQueue.name": "ingress"});

    // while here, also assert that current queue dwell time is reflect in the total
    assert.eq(opsBeforeAdmission.length, 1);
    const opBeforeAdmission = opsBeforeAdmission[0];
    assert.gte(opBeforeAdmission.queues.ingress.totalTimeQueuedMicros,
               opBeforeAdmission.currentQueue.timeQueuedMicros);

    // make sure the operation hangs after it was admitted
    const fp = configureFailPoint(conn, "waitAfterCommandFinishesExecution", {commands: ["count"]});

    // unblock ingress admission
    assert.commandWorked(
        db.adminCommand({setParameter: 1, ingressAdmissionControllerTicketPoolSize: 1}));

    // confirm that our operation is no longer waiting for ingress admission
    const opsAtferAdmission = waitForCurOpByComment(db, kComment, {currentQueue: null});

    // while here, validate that the operation was admitted and is holding a ticket
    assert.eq(opsAtferAdmission.length, 1);
    const opAtferAdmission = opsAtferAdmission[0];
    assert.gte(opAtferAdmission.queues.ingress.admissions, 1);
    assert(opAtferAdmission.queues.ingress.isHoldingTicket);

    fp.off();
    parallelShell();
}

/**
 * Test that the time spent waiting for ingress admission is reported in slow query log.
 */
function testSlowQueryLog(conn, db, collName) {
    const kDelayMillis = 50;
    const kDelayMicros = kDelayMillis * 1000;
    const kComment = "ingress admission queue curop test slow query";

    // clear server log to make sure there's only one slow log entry when we check
    assert.commandWorked(db.adminCommand({clearLog: "global"}));

    // run a parallel shell that can wait for admission control
    const parallelShell =
        startParallelShell(funWithArgs((dbName, collName, comment) => {
                               const testDB = db.getSiblingDB(dbName);

                               // report every command as slow
                               testDB.setProfilingLevel(0, -1);

                               // make the next controlled command wait for ingress admission
                               assert.commandWorked(testDB.adminCommand(
                                   {setParameter: 1, ingressAdmissionControllerTicketPoolSize: 0}));

                               // wait until the command has been admitted
                               assert.commandWorked(testDB.runCommand({count: collName, comment}));
                           }, db.getName(), collName, kComment), conn.port, false);

    // confirm that our operation is waiting for ingress admission
    waitForCurOpByComment(db, kComment, {"currentQueue.name": "ingress"});

    // make sure the reported ingress admission wait time will be at least kDelayMillis
    sleep(kDelayMillis);

    // unblock ingress admission
    assert.commandWorked(
        db.adminCommand({setParameter: 1, ingressAdmissionControllerTicketPoolSize: 1}));

    // wait until the parallel shell is finished to stop reporting all commands as slow
    parallelShell();
    db.setProfilingLevel(0, 100);

    // verify that the reported ingress admission wait time is at least kDelayMillis
    const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
    const line = findMatchingLogLine(log, {id: 51803, command: "count"});
    assert.neq(line, null);
    const entry = JSON.parse(line);
    assert.eq(entry.attr.queues.ingress.admissions, 1);
    assert.gte(entry.attr.queues.ingress.totalTimeQueuedMicros, kDelayMicros);
    assert(entry.attr.currentQueue == null, 'expected no current queue in slow query logs');
}

/**
 * Test that the operations waiting for ingress admission will respect maxTimeMS.
 */
function testMaxTimeMS(db, collName) {
    // ensure controlled operations work with the default pool size
    assert.commandWorked(db.runCommand({count: collName}));

    // block all controlled operations indefinitely
    assert.commandWorked(
        db.adminCommand({setParameter: 1, ingressAdmissionControllerTicketPoolSize: 0}));

    // ensure controlled operations time out while queued
    const cmdRes = db.runCommand({count: collName, maxTimeMS: 500});
    assert.commandFailedWithCode(cmdRes, ErrorCodes.MaxTimeMSExpired);

    // stop blocking controlled operations so we can stop the runner
    assert.commandWorked(
        db.adminCommand({setParameter: 1, ingressAdmissionControllerTicketPoolSize: 1}));
}

function runTests() {
    const mongodOptions = {setParameter: {ingressAdmissionControllerTicketPoolSize: 1}};
    const conn = MongoRunner.runMongod(mongodOptions);

    const db = conn.getDB(`${jsTest.name()}_db`);
    const collName = `${jsTest.name()}_coll`;
    db.getCollection(collName).drop();

    testPoolSizeValidation(db);
    testCurrentOp(conn, db, collName);
    testSlowQueryLog(conn, db, collName);
    testMaxTimeMS(db, collName);

    MongoRunner.stopMongod(conn);
}

runTests();
