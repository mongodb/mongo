/**
 * Tests with concurrent DDL operation during release memory yield.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const db = conn.getDB(dbName);

assert.commandWorked(db.setProfilingLevel(1, {slowms: -1}));

function initCollection(coll) {
    assert(coll.drop());
    assert.commandWorked(coll.insert(Array.from({length: 10}, (_, i) => ({_id: i, a: i, b: i}))));
}

function initCursorId(coll) {
    const cursor =
        coll.find({a: {$gte: 0}, b: {$gte: 0}}, {_id: 0, b: 0}).sort({a: 1}).batchSize(1);
    assert(cursor.hasNext());
    return cursor.getId();
}

// Set it to a low value so release memory will yield.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

initCollection(db.coll1);
initCollection(db.coll2);

/*
 * Drop the collection during release memory yield.
 */
{
    const cursor1 = initCursorId(db.coll1);
    const cursor2 = initCursorId(db.coll2);

    const fp =
        configureFailPoint(conn, "setYieldAllLocksHang", {namespace: db.coll1.getFullName()});

    const awaitShell = startParallelShell(`
    import {assertReleaseMemoryFailedWithCode, assertReleaseMemoryWorked} from "jstests/libs/release_memory_util.js";

    const res = db.runCommand({releaseMemory: [${cursor1}, ${cursor2}]});
    jsTest.log("Release memory result: " + tojson(res));

    assertReleaseMemoryFailedWithCode(
        res, ${cursor1}, [ErrorCodes.NamespaceNotFound, ErrorCodes.QueryPlanKilled]);
    assertReleaseMemoryWorked(res, ${cursor2});
    `, conn.port);

    fp.wait();
    assert(db.coll1.drop());
    fp.off();
    awaitShell();

    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const slowQueryLogLine =
        findMatchingLogLine(globalLog.log, {msg: "Slow query", command: "releaseMemory"});
    assert(slowQueryLogLine, "Failed to find a log line for releaseMemory command");
    assert.gt(JSON.parse(slowQueryLogLine).attr.numYields, 0, slowQueryLogLine);
}

/*
 * Interrupt release memory during yield.
 */
{
    initCollection(db.coll1);
    const cursor1 = initCursorId(db.coll1);
    const cursor2 = initCursorId(db.coll2);

    const fp =
        configureFailPoint(conn, "setYieldAllLocksHang", {namespace: db.coll1.getFullName()});

    const awaitShell = startParallelShell(`
    import {assertReleaseMemoryFailedWithCode} from "jstests/libs/release_memory_util.js";

    const res = db.runCommand({releaseMemory: [${cursor1}, ${cursor2}]});
    jsTest.log("Release memory result: " + tojson(res));

    assertReleaseMemoryFailedWithCode(res, ${cursor1}, [ErrorCodes.Interrupted]);
    assertReleaseMemoryFailedWithCode(res, ${cursor2}, [ErrorCodes.Interrupted]);
    `, conn.port);

    fp.wait();

    let opId = null;
    assert.soon(function() {
        const ops = db.getSiblingDB("admin")
                        .aggregate([
                            {$currentOp: {allUsers: true, localOps: true}},
                            {
                                $match: {
                                    numYields: {$gt: 0},
                                    op: "command",
                                    "command.releaseMemory": {$exists: true},
                                }
                            }
                        ])
                        .toArray();

        if (ops.length > 0) {
            assert.eq(ops.length, 1);
            opId = ops[0].opid;
            return true;
        }

        return false;
    });
    db.killOp(opId);

    fp.off();
    awaitShell();
}

MongoRunner.stopMongod(conn);
