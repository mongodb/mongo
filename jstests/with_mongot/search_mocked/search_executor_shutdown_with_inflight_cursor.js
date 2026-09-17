/**
 * Verify that mongod shutdown completes via the search task executor timeout backstop when a cursor
 * is pinned by an in-flight getMore. Because a pinned cursor cannot be disposed during shutdown, the
 * executor wait is the only thing bounding shutdown — this exercises the fix's timeout-backstop path
 * rather than the idle-disposal path covered by search_executor_shutdown_with_idle_cursor.js.
 *
 * @tags: [ requires_fcv_71 ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Runs in a parallel shell. The getMore blocks at the getMoreHangAfterPinCursor failpoint, so we
// expect it to be interrupted when mongod shuts down; swallow that error so the shell exits cleanly.
// The cursor must be created and tied to this same session id so the getMore can legitimately extend
// it (the parallel shell has a distinct implicit session otherwise).
const doGetMore = function (dbName, collName, cursorId, sessionId) {
    const testDb = db.getSiblingDB(dbName);
    try {
        const res = testDb.runCommand({
            getMore: cursorId,
            collection: collName,
            batchSize: 1,
            lsid: sessionId,
        });
        jsTest.log.info("in-flight getMore returned", {res});
    } catch (e) {
        jsTest.log.info("in-flight getMore ended during shutdown (expected)", {error: e});
    }
};

describe("search executor shutdown", function () {
    it("completes via the timeout backstop when a cursor is pinned by an in-flight getMore", function () {
        const dbName = jsTestName();
        const collName = jsTestName();

        const mongotmock = new MongotMock();
        mongotmock.start();
        const mongotConn = mongotmock.getConnection();

        const conn = MongoRunner.runMongod({
            setParameter: {
                mongotHost: mongotConn.host,
                // The cursor is pinned during shutdown (it cannot be disposed), so the executor wait
                // is what bounds shutdown. Keep the timeout small enough that the test is fast, but
                // large enough to clearly distinguish from a prompt (< ~1s) shutdown.
                searchTaskExecutorShutdownTimeoutMS: 8000,
            },
        });

        const db = conn.getDB(dbName);
        const coll = db[collName];

        assert.commandWorked(coll.insert({_id: 1, title: "cakes"}));
        assert.commandWorked(coll.insert({_id: 2, title: "cookies and cakes"}));
        assert.commandWorked(coll.insert({_id: 3, title: "vegetables"}));

        const collUUID = db.getCollectionInfos({name: collName})[0].info.uuid;
        const searchQuery = {query: "cakes", path: "title"};

        // mongot returns all results in one batch with cursor id 0 (exhausted on mongot). The
        // small batchSize below means mongod keeps a client-visible cursor open, and the pipeline
        // (and its TaskExecutorCursor, which pins the mongot executor) stays alive for that cursor.
        const searchCmd = {
            search: collName,
            collectionUUID: collUUID,
            query: searchQuery,
            $db: dbName,
        };
        const history = [
            {
                expectedCommand: searchCmd,
                response: {
                    ok: 1,
                    cursor: {
                        id: NumberLong(0),
                        ns: coll.getFullName(),
                        nextBatch: [
                            {_id: 1, $searchScore: 0.99},
                            {_id: 2, $searchScore: 0.65},
                            {_id: 3, $searchScore: 0.32},
                        ],
                    },
                },
            },
        ];
        // cursorId is just the (non-zero) key the mock stores this history under. The response's
        // own cursor.id is 0 (mongot is exhausted), so mongod sends neither a getMore nor a
        // killCursors. mongod's client-visible cursor (id != 0) will drive the in-flight getMore.
        mongotmock.setMockResponses(history, NumberLong(1));

        // Open a $search cursor via a raw aggregate command so we can grab the server-side cursor id
        // needed to issue the background getMore. Use an explicit session so the background getMore
        // can extend this cursor (the parallel shell has a different implicit session otherwise).
        const session = db.getMongo().startSession();
        const aggregateResult = session.getDatabase(dbName).runCommand({
            aggregate: collName,
            pipeline: [{$search: searchQuery}],
            cursor: {batchSize: 1},
        });
        assert.commandWorked(aggregateResult);
        const cursorId = aggregateResult.cursor.id;
        assert.neq(cursorId, NumberLong(0), "expected a non-exhausted client-visible cursor");
        assert.eq(
            aggregateResult.cursor.firstBatch.length,
            1,
            "expected one document in first batch",
        );
        assert.eq(aggregateResult.cursor.firstBatch[0], {_id: 1, title: "cakes"});

        assert.gte(db.serverStatus().metrics.cursor.open.total, 1, "expected an open idle cursor");

        // Pause a getMore on this cursor right after it is pinned. This forces the cursor into the
        // "in-flight" (pinned) state that disposal must skip, so we can deterministically test the
        // executor timeout backstop during shutdown.
        const getMoreFailpoint = configureFailPoint(db, "getMoreHangAfterPinCursor");

        const joinGetMore = startParallelShell(
            funWithArgs(doGetMore, dbName, collName, cursorId, session.getSessionId()),
            conn.port,
        );

        // Block until the getMore has reached the failpoint, i.e. the cursor is genuinely pinned.
        getMoreFailpoint.wait({maxTimeMS: 60000});

        // Shut down mongod. Because the cursor is pinned it cannot be disposed, so the mongot task
        // executor reference is still held and shutdown is bounded only by the executor timeout.
        const start = Date.now();
        const exitCode = MongoRunner.stopMongod(conn);
        const elapsedMs = Date.now() - start;
        jsTest.log.info("mongod shutdown complete", {elapsedMs, exitCode});

        assert.eq(exitCode, MongoRunner.EXIT_CLEAN, "mongod did not shut down cleanly");
        // The pinned cursor cannot be disposed, so shutdown must have waited for the executor. Assert
        // only a lower bound: a prompt shutdown (< 1s) would mean the pinned cursor was wrongly
        // disposed. Under load, shutdown can only get slower, so this lower bound never flakes.
        assert.gte(elapsedMs, 5000, "shutdown was prompt, implying a pinned cursor was disposed");

        joinGetMore();
        mongotmock.stop();
    });
});
