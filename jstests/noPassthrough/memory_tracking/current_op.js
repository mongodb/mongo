/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported via $currentOp for those stages which track memory.
 * @tags: [
 *   requires_getmore,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const serverParams = {
    setParameter: {
        // Needed to ensure we test the right group stage.
        internalQueryFrameworkControl: "forceClassicEngine",
        // Needed to avoid spilling to disk, which changes memory metrics.
        allowDiskUseByDefault: false,
    },
};

const dbName = jsTestName();
const collName = "test";

function runStandaloneTest(dbName, collName) {
    jsTest.log.info("Running standalone test");
    const conn = MongoRunner.runMongod(serverParams);
    const db = conn.getDB(dbName);
    const coll = db[collName];
    coll.drop();
    insertData(coll);
    runTest(conn, db, coll);
    MongoRunner.stopMongod(conn);
}

function runShardedTest(dbName, collName) {
    jsTest.log.info("Running sharded test");
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1},
        other: {mongosOptions: serverParams, rsOptions: serverParams},
    });
    const mongos = st.s0;
    const adminDb = mongos.getDB("admin");
    const namespace = dbName + "." + collName;

    // Shard the collection.
    assert.commandWorked(adminDb.runCommand({enableSharding: dbName}));
    assert.commandWorked(adminDb.runCommand({shardCollection: namespace, key: {_id: "hashed"}}));

    const db = mongos.getDB(dbName);
    const coll = db[collName];
    insertData(coll);
    runTest(mongos, db, coll);
    st.stop();
}

function insertData(coll) {
    let rows = [];
    for (let i = 0; i < 100; ++i) {
        rows.push({_id: i});
    }
    assert.commandWorked(coll.insertMany(rows));
}

function runTest(conn, db, coll) {
    const shouldAppear = FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking");
    checkCurrentOpMemoryTracking(
        "group",
        conn,
        db,
        coll,
        [{$group: {_id: {$mod: ["$_id", 3]}, count: {$sum: 1}}}],
        shouldAppear,
    );
}

function assertCurrentOpOutput(shouldAppear, curOpDoc) {
    if (shouldAppear) {
        assert(curOpDoc.hasOwnProperty("inUseTrackedMemBytes"), tojson(curOpDoc));
        assert.gt(curOpDoc.inUseTrackedMemBytes, 0);
        assert(curOpDoc.hasOwnProperty("peakTrackedMemBytes"), tojson(curOpDoc));
        assert.gt(curOpDoc.peakTrackedMemBytes, 0);
    } else {
        assert(!curOpDoc.hasOwnProperty("inUseTrackedMemBytes"), tojson(curOpDoc));
        assert(!curOpDoc.hasOwnProperty("peakTrackedMemBytes"), tojson(curOpDoc));
    }
}

export function checkCurrentOpMemoryTracking(stageName, conn, db, coll, pipeline, shouldAppear) {
    jsTest.log.info(
        `Checking $currentOp for stage ${stageName}. Memory tracking metrics should ` +
            (shouldAppear ? "appear" : "not appear"),
    );

    // Run a pipeline with a small batch size, to ensure that a cursor is created and we can call
    // getMore().
    const session = db.getMongo().startSession();
    const sessionDb = session.getDatabase(db.getName());
    let cursorId = assert.commandWorked(
        sessionDb.runCommand({
            aggregate: coll.getName(),
            pipeline,
            allowDiskUse: false,
            cursor: {batchSize: 1},
        }),
    ).cursor.id;
    assert.neq(cursorId, NumberLong(0));

    // Create a failpoint to block the getMore command.
    const failPoint = configureFailPoint(conn, "waitWithPinnedCursorDuringGetMoreBatch");

    // getMore() has to be invoked within the same session in which the cursor was created.
    const sessionId = session.getSessionId();
    let getMoreShell = startParallelShell(
        funWithArgs(
            function (dbName, cursorId, collName, sessionId) {
                const testDb = db.getSiblingDB(dbName);
                assert.commandWorked(
                    testDb.runCommand({getMore: cursorId, collection: collName, lsid: sessionId, batchSize: 1}),
                );
            },
            db.getName(),
            NumberLong(cursorId),
            coll.getName(),
            sessionId,
        ),
        conn.port,
    );

    // Wait for the getMore to be blocked on the failpoint.
    failPoint.wait();

    // Find the current operation for the getMore blocked command. We need to specify "localOps" to
    // see operations on mongos. This flag has no effect on a standalone mongod.
    let curOpDocs = db
        .getSiblingDB("admin")
        .aggregate([{$currentOp: {localOps: true}}, {$match: {"lsid.id": sessionId.id}}])
        .toArray();
    assert.eq(curOpDocs.length, 1, "Expected one current operation for the getMore command");

    assertCurrentOpOutput(shouldAppear, curOpDocs[0]);

    // Unblock the getMore command and wait for it to finish.
    failPoint.off();
    getMoreShell();

    // Now check that we also report memory stats for an idle cursor.
    curOpDocs = db
        .getSiblingDB("admin")
        .aggregate([
            {$currentOp: {localOps: true, idleCursors: true}},
            {$match: {"lsid.id": sessionId.id, "type": "idleCursor"}},
        ])
        .toArray();
    assert.eq(curOpDocs.length, 1, "Expected one idle cursor for unfinished query");
    assertCurrentOpOutput(shouldAppear, curOpDocs[0]);

    session.endSession();
}

runStandaloneTest(dbName, collName);
runShardedTest(dbName, collName);
