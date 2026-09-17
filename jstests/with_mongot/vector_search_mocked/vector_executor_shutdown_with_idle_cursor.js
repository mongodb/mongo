/**
 * Verify that mongod shuts down promptly when an idle $vectorSearch cursor holds a reference to the
 * mongot search task executor. This covers the VectorSearchStage path (as opposed to the
 * $search path covered by search_executor_shutdown_with_idle_cursor.js) that also pins the mongot
 * executor on an open cursor.
 *
 * @tags: [ requires_fcv_71 ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock,
    mongotResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

describe("vector search executor shutdown", function () {
    it("shuts down promptly when an idle $vectorSearch cursor holds an executor reference", function () {
        const dbName = jsTestName();
        const collName = jsTestName();

        const mongotmock = new MongotMock();
        mongotmock.start();
        const mongotConn = mongotmock.getConnection();

        const conn = MongoRunner.runMongod({
            setParameter: {
                mongotHost: mongotConn.host,
                // Use a long timeout (not infinite) so the test fails fast with a clear message if
                // the idle cursor is not disposed promptly, rather than hanging until the timeout.
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

        // mongot returns all results in one batch with cursor id 0 (exhausted on mongot). mongod
        // still keeps a client-visible cursor open because of the small batchSize below, and the
        // pipeline (and its TaskExecutorCursor, which pins the mongot executor) stays alive for that
        // open cursor.
        const expectedCommand = mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            numCandidates,
            index,
            limit,
            collName,
            dbName,
            collectionUUID: collUUID,
        });
        const history = [
            {
                expectedCommand,
                response: mongotResponseForBatch(
                    [
                        {_id: 1, $vectorSearchScore: 0.654},
                        {_id: 2, $vectorSearchScore: 0.321},
                        {_id: 3, $vectorSearchScore: 0.123},
                    ],
                    NumberLong(0),
                    coll.getFullName(),
                    1,
                ),
            },
        ];
        mongotmock.setMockResponses(history, NumberLong(1));

        // Open a $vectorSearch cursor and read only the first document, leaving the cursor idle on
        // mongod.
        const pipeline = [
            {$vectorSearch: {queryVector, path, numCandidates, limit, index}},
            {$project: {_id: 1}},
        ];
        const cursor = coll.aggregate(pipeline, {cursor: {batchSize: 1}});
        assert(cursor.hasNext());
        assert.eq(cursor.next(), {_id: 1});

        // The cursor is still open (not exhausted) and thus still pinning the mongot executor.
        assert.gte(db.serverStatus().metrics.cursor.open.total, 1, "expected an open idle cursor");

        // Stop mongod. The executor wait is set long enough that the only thing that makes shutdown
        // prompt is disposing the idle cursor that holds the executor reference.
        const start = Date.now();
        const exitCode = MongoRunner.stopMongod(conn);
        const elapsedMs = Date.now() - start;
        jsTest.log.info("mongod shutdown complete", {elapsedMs, exitCode});

        assert.eq(exitCode, MongoRunner.EXIT_CLEAN, "mongod did not shut down cleanly");
        // Clean shutdown should be prompt; the 10s executor timeout is a backstop that should never
        // fire if the idle cursor is disposed correctly.
        assert.lt(elapsedMs, 3000, "mongod shutdown was slow, indicating it hung on the executor");

        mongotmock.stop();
    });
});
