/**
 * Tests that release memory doesn't prevent DDL operation and can be interrupted.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const db = conn.getDB(dbName);

assert.commandWorked(db.setProfilingLevel(1, {slowms: -1}));

function initCollection(coll) {
    assert(coll.drop());
    assert.commandWorked(coll.insert(Array.from({length: 10}, (_, i) => ({_id: i, a: i, b: i}))));
}

function initCursor(coll) {
    const cursor =
        coll.find({a: {$gte: 0}, b: {$gte: 0}}, {_id: 0, b: 0}).sort({a: 1}).batchSize(1);
    assert(cursor.hasNext());
    return cursor;
}

// Set it to a low value so release memory will yield.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

initCollection(db.coll1);
initCollection(db.coll2);

/*
 * Drop the collection during release memory yield.
 */
{
    const expectedResult = initCursor(db.coll2).toArray();

    const cursor1 = initCursor(db.coll1);
    const cursor2 = initCursor(db.coll2);

    const fp = configureFailPoint(
        conn, "setInterruptOnlyPlansCheckForInterruptHang", {namespace: db.coll1.getFullName()});

    const awaitShell = startParallelShell(`
    import {assertReleaseMemoryFailedWithCode, assertReleaseMemoryWorked} from "jstests/libs/release_memory_util.js";

    const res = db.runCommand({releaseMemory: [${cursor1.getId()}, ${cursor2.getId()}]});
    jsTest.log("Release memory result: " + tojson(res));

    assertReleaseMemoryWorked(res, ${cursor1.getId()});
    assertReleaseMemoryWorked(res, ${cursor2.getId()});
    `, conn.port);

    fp.wait();
    assert(db.coll1.drop());
    fp.off();
    awaitShell();

    assert.throwsWithCode(() => cursor1.toArray(),
                          [ErrorCodes.NamespaceNotFound, ErrorCodes.QueryPlanKilled]);
    assertArrayEq({actual: cursor2.toArray(), expected: expectedResult});
}

/*
 * Interrupt release memory during yield.
 */
{
    initCollection(db.coll1);

    const expectedResult = initCursor(db.coll1).toArray();

    const cursor1 = initCursor(db.coll1);
    const cursor2 = initCursor(db.coll2);

    const fp = configureFailPoint(
        conn, "setInterruptOnlyPlansCheckForInterruptHang", {namespace: db.coll1.getFullName()});

    const awaitShell = startParallelShell(`
    import {assertReleaseMemoryFailedWithCode} from "jstests/libs/release_memory_util.js";

    const res = db.runCommand({releaseMemory: [${cursor1.getId()}, ${cursor2.getId()}]});
    jsTest.log("Release memory result: " + tojson(res));

    assertReleaseMemoryFailedWithCode(res, ${cursor1.getId()}, [ErrorCodes.Interrupted]);
    `, conn.port);

    fp.wait();

    let opId = null;
    assert.soon(function() {
        const ops = db.getSiblingDB("admin")
                        .aggregate([
                            {$currentOp: {allUsers: true, localOps: true}},
                            {
                                $match: {
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

    // releaseMemory was interrupted when working on cursor1, so it should be destroyed.
    assert.throwsWithCode(() => cursor1.toArray(), [ErrorCodes.CursorNotFound]);
    // cursor2 should still be valid, because releaseMemory never pinned it.
    assertArrayEq({actual: cursor2.toArray(), expected: expectedResult});
}

MongoRunner.stopMongod(conn);
