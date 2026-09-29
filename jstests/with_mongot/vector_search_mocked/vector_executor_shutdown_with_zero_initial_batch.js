/**
 * Verify that mongod shuts down promptly when a $vectorSearch cursor is registered with batchSize: 0
 * and never executes. Such a cursor never sets OpDebug::mongotCursorId, but its $vectorSearch stage
 * still retains the mongot task executor from construction, so shutdown must dispose it. This
 * covers the $vectorSearch path (as opposed to the $search path covered by
 * search_executor_shutdown_with_zero_initial_batch.js).
 *
 * @tags: [ requires_fcv_91 ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

describe("vector search executor shutdown", function () {
    it("shuts down promptly when a $vectorSearch cursor is registered with batchSize: 0", function () {
        const dbName = jsTestName();
        const collName = jsTestName();

        const mongotmock = new MongotMock();
        mongotmock.start();
        const mongotConn = mongotmock.getConnection();

        const conn = MongoRunner.runMongod({
            setParameter: {
                mongotHost: mongotConn.host,
                // Use a long timeout (not infinite) so the test fails fast with a clear message if
                // the cursor is not disposed promptly, rather than hanging until the timeout.
                searchTaskExecutorShutdownTimeoutMS: 10000,
            },
        });

        const db = conn.getDB(dbName);
        const coll = db[collName];

        assert.commandWorked(coll.insert({_id: 1, title: "cakes"}));
        assert.commandWorked(coll.insert({_id: 2, title: "cookies and cakes"}));
        assert.commandWorked(coll.insert({_id: 3, title: "vegetables"}));

        const collUUID = db.getCollectionInfos({name: collName})[0].info.uuid;
        const queryVector = [1.0, 2.0, 3.0];
        const path = "title";
        const numCandidates = 10;
        const limit = 3;
        const index = "index";

        // No mongot response is queued: with batchSize: 0 the pipeline never executes, so no
        // vectorSearch command is ever sent to mongot. If one were sent, the mock would fail the
        // test on the unexpected command. (The $search counterpart,
        // search_executor_shutdown_with_zero_initial_batch.js, does queue a response: its stage
        // establishes the mongot cursor eagerly at registration.)

        // Open a $vectorSearch cursor with batchSize: 0. The first batch is empty and the pipeline
        // is not executed, but the cursor is still live and its stage holds the mongot executor.
        const res = assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$vectorSearch: {queryVector, path, numCandidates, index, limit}}],
                cursor: {batchSize: 0},
            }),
        );
        assert.eq(res.cursor.firstBatch.length, 0, "expected an empty first batch");
        assert.neq(res.cursor.id, NumberLong(0), "expected a live cursor");

        // The cursor is still open and thus still retaining the mongot executor.
        assert.gte(db.serverStatus().metrics.cursor.open.total, 1, "expected an open idle cursor");

        // Stop mongod. The executor wait is set long enough that the only thing that makes shutdown
        // prompt is disposing the cursor that holds the executor reference.
        const start = Date.now();
        const exitCode = MongoRunner.stopMongod(conn);
        const elapsedMs = Date.now() - start;
        jsTest.log.info("mongod shutdown complete", {elapsedMs, exitCode});

        assert.eq(exitCode, MongoRunner.EXIT_CLEAN, "mongod did not shut down cleanly");
        // Prompt shutdown should be well under the 10s executor timeout; if the cursor were not
        // disposed, shutdown would wait out the backstop (or hang), taking 10s or more.
        assert.lt(elapsedMs, 8000, "mongod shutdown was slow, indicating it hung on the executor");

        mongotmock.stop();
    });
});
