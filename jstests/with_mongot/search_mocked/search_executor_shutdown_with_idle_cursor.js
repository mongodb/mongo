import {describe, it} from "jstests/libs/mochalite.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

describe("search executor shutdown", function () {
    it("shuts down promptly when an idle $search cursor holds an executor reference", function () {
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
        const searchQuery = {query: "cakes", path: "title"};

        // mongot returns all results in one batch with cursor id 0 (exhausted on mongot). mongod
        // still keeps a client-visible cursor open because of the small batchSize below, and the
        // pipeline (and its TaskExecutorCursor, which pins the mongot executor) stays alive for that
        // open cursor.
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
        // killCursors.
        mongotmock.setMockResponses(history, NumberLong(1));

        // Open a $search cursor and read only the first document, leaving the cursor idle on mongod.
        const cursor = coll.aggregate([{$search: searchQuery}], {cursor: {batchSize: 1}});
        assert(cursor.hasNext());
        assert.eq(cursor.next(), {_id: 1, title: "cakes"});

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
