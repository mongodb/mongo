/**
 * Verify that mongod shuts down promptly when a $search cursor is registered with batchSize: 0 and
 * never executes. Such a cursor never sets OpDebug::mongotCursorId, but its search stage still
 * retains the mongot task executor from construction, so shutdown must dispose it. This covers the
 * $search path; the $vectorSearch equivalent is covered by
 * vector_executor_shutdown_with_zero_initial_batch.js.
 *
 * @tags: [ requires_fcv_91 ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

describe("search executor shutdown", function () {
    it("shuts down promptly when a $search cursor is registered with batchSize: 0", function () {
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
        const searchQuery = {query: "cakes", path: "title"};

        // mongod establishes the mongot cursor when registering the aggregate (even with a
        // batchSize of 0, the search command is sent to mongot and this response is consumed), but
        // the pipeline itself is never executed, so OpDebug::mongotCursorId is never set. The
        // open cursor keeps the mongot task executor alive for shutdown to reclaim.
        // (The $vectorSearch counterpart, vector_executor_shutdown_with_zero_initial_batch.js,
        // queues no response: its stage establishes the mongot cursor lazily instead.)
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
        mongotmock.setMockResponses(history, NumberLong(1));

        // Open a $search cursor with batchSize: 0. The first batch is empty and the pipeline is not
        // executed, but the cursor is still live and its search stage holds the mongot executor.
        const res = assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$search: searchQuery}],
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
