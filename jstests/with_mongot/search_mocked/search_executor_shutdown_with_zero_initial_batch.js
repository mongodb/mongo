/**
 * Verify that mongod shuts down promptly when a $search cursor is registered with batchSize: 0 and
 * never executes. Such a cursor never sets OpDebug::mongotCursorId, but its search stage still
 * retains the mongot task executor from construction, so shutdown must dispose it. This covers both
 * a $search pipeline on a collection and an aggregation on a view whose definition contains $search
 * (where the mongot stage comes from the view definition and only appears in the resolved execution
 * pipeline). The $vectorSearch equivalent is covered by
 * vector_executor_shutdown_with_zero_initial_batch.js.
 *
 * @tags: [ requires_fcv_91 ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

describe("search executor shutdown", function () {
    let mongotmock;
    let conn;
    let mongodStopped;

    beforeEach(function () {
        mongotmock = new MongotMock();
        mongotmock.start();

        conn = MongoRunner.runMongod({
            setParameter: {
                mongotHost: mongotmock.getConnection().host,
                // Use a long timeout (not infinite) so the test fails fast with a clear message if
                // the cursor is not disposed promptly, rather than hanging until the timeout.
                searchTaskExecutorShutdownTimeoutMS: 10000,
            },
        });
        mongodStopped = false;
    });

    afterEach(function () {
        if (conn && !mongodStopped) {
            MongoRunner.stopMongod(conn);
        }
        mongotmock.stop();
    });

    // Inserts the seed documents and queues the mock response that mongod consumes while
    // registering the aggregate: even with a batchSize of 0, the search command is sent to mongot
    // and this response is consumed, but the pipeline itself is never executed, so
    // OpDebug::mongotCursorId is never set. The open cursor keeps the mongot task executor alive
    // for shutdown to reclaim.
    function seedCollectionAndQueueMockResponse(db, collName) {
        const coll = db[collName];
        assert.commandWorked(coll.insert({_id: 1, title: "cakes"}));
        assert.commandWorked(coll.insert({_id: 2, title: "cookies and cakes"}));
        assert.commandWorked(coll.insert({_id: 3, title: "vegetables"}));

        const searchQuery = {query: "cakes", path: "title"};
        const searchCmd = {
            search: collName,
            collectionUUID: db.getCollectionInfos({name: collName})[0].info.uuid,
            query: searchQuery,
            $db: db.getName(),
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
        return searchQuery;
    }

    // Runs the aggregate with batchSize: 0. The first batch is empty and the pipeline is not
    // executed, but the cursor is still live and its search stage holds the mongot executor.
    function openIdleCursorAndAssertIdle(db, aggregateCmd) {
        const res = assert.commandWorked(db.runCommand({...aggregateCmd, cursor: {batchSize: 0}}));
        assert.eq(res.cursor.firstBatch.length, 0, "expected an empty first batch");
        assert.neq(res.cursor.id, NumberLong(0), "expected a live cursor");

        // The cursor is still open and thus still retaining the mongot executor.
        assert.gte(db.serverStatus().metrics.cursor.open.total, 1, "expected an open idle cursor");
    }

    // Stops mongod and asserts the shutdown was prompt. The executor wait is set long enough that
    // the only thing that makes shutdown prompt is disposing the cursor that holds the executor
    // reference.
    function assertPromptShutdown() {
        const start = Date.now();
        const exitCode = MongoRunner.stopMongod(conn);
        mongodStopped = true;
        const elapsedMs = Date.now() - start;
        jsTest.log.info("mongod shutdown complete", {elapsedMs, exitCode});

        assert.eq(exitCode, MongoRunner.EXIT_CLEAN, "mongod did not shut down cleanly");
        // Prompt shutdown should be well under the 10s executor timeout; if the cursor were not
        // disposed, shutdown would wait out the backstop (or hang), taking 10s or more.
        assert.lt(elapsedMs, 8000, "mongod shutdown was slow, indicating it hung on the executor");
    }

    it("shuts down promptly when a $search cursor is registered with batchSize: 0", function () {
        const dbName = jsTestName();
        const collName = jsTestName();
        const db = conn.getDB(dbName);

        const searchQuery = seedCollectionAndQueueMockResponse(db, collName);

        // The $vectorSearch counterpart, vector_executor_shutdown_with_zero_initial_batch.js,
        // queues no response: its stage establishes the mongot cursor lazily instead.
        openIdleCursorAndAssertIdle(db, {
            aggregate: collName,
            pipeline: [{$search: searchQuery}],
        });

        assertPromptShutdown();
    });

    it("shuts down promptly when a view $search cursor is registered with batchSize: 0", function () {
        const dbName = jsTestName();
        const collName = jsTestName();
        const viewName = collName + "_view";
        const db = conn.getDB(dbName);

        const searchQuery = seedCollectionAndQueueMockResponse(db, collName);

        assert.commandWorked(db.createView(viewName, collName, [{$search: searchQuery}]));

        // mongod desugars the view's $search into a search command against the underlying
        // collection. The mongot stage comes from the view definition, so it only appears in the
        // resolved execution pipeline, not in the caller's original pipeline.
        openIdleCursorAndAssertIdle(db, {aggregate: viewName, pipeline: []});

        assertPromptShutdown();
    });
});
