/**
 * Tests that release memory can be interrupted on a sharded cluster.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1},
    other: {
        rsOptions: {
            setParameter: {
                internalQueryExecYieldIterations: 1,
            }
        }
    }
});

const dbName = jsTestName();
const db = st.s.getDB(dbName);

db.setLogLevel(2, "query");
st.rs0.getPrimary().getDB(dbName).setProfilingLevel(1, {slowms: -1});

function initCollection(coll) {
    assert(coll.drop());
    const kDocCount = 1024;
    // Total size of the collection should be ~100 MB so that the router can't buffer the whole
    // shard output.
    const padding = "X".repeat((128 * 1024 * 1024) / kDocCount);
    assert.commandWorked(coll.insert(
        Array.from({length: kDocCount}, (_, i) => ({_id: i, a: i, b: i, padding: padding}))));
    st.shardColl(
        coll.getName(), {_id: 1}, {_id: kDocCount / 2}, {_id: 0}, dbName, true /*waitForDelete*/);
}

function initCursorFind(coll) {
    const cursor =
        coll.find({a: {$gte: 0}, b: {$gte: 0}}, {_id: 0, b: 0}).sort({a: 1}).batchSize(1);
    assert(cursor.hasNext());
    return cursor;
}

initCollection(db.coll);

function runInterruptTest(createFailpoint, interruptOperation) {
    const expectedResult = initCursorFind(db.coll).toArray();

    const cursor1 = initCursorFind(db.coll);
    const cursor2 = initCursorFind(db.coll);

    const fp = createFailpoint();

    const awaitShell = startParallelShell(`
import {assertReleaseMemoryFailedWithCode} from "jstests/libs/release_memory_util.js";

const res = db.runCommand({releaseMemory: [${cursor1.getId()}, ${cursor2.getId()}]});
jsTest.log("Release memory result: " + tojson(res));

assertReleaseMemoryFailedWithCode(res, ${cursor1.getId()}, [ErrorCodes.Interrupted]);
`, st.s.port);

    fp.wait();
    interruptOperation();
    fp.off();

    awaitShell();

    assert.throwsWithCode(() => cursor1.toArray(), [ErrorCodes.CursorNotFound]);
    assertArrayEq({actual: cursor2.toArray(), expected: expectedResult});
}

function killReleaseMemoryCommandOnMongos() {
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
}

function configureMongodFailPoint() {
    return configureFailPoint(st.rs0.getPrimary().getDB(dbName),
                              "releaseMemoryHangAfterPinCursor",
                              {namespace: db.coll.getFullName()});
}

runInterruptTest(configureMongodFailPoint, killReleaseMemoryCommandOnMongos);

st.stop();
