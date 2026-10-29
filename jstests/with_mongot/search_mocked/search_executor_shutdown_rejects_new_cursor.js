/**
 * Verify that a $search issued while the search task executor is shutting down is refused cleanly
 * (ShutdownInProgress), instead of establishing a new TaskExecutorCursor/PinnedConnectionTaskExecutor
 * that could outlive the executor.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Name of the collection the parallel shell inserts into to signal that it has connected, so the
// parent knows it is safe to start shutting mongod down (which closes the listener).
const kChildReadyColl = "search_shutdown_child_ready";

// Runs in a parallel shell. First signals that it is connected, then waits until mongod's shutdown
// is paused after draining the pinned executors but before shutting down the underlying executor,
// then attempts a $search and verifies it is refused. Always releases the fail point so shutdown
// can proceed even if the assertion fails.
const doSearchDuringShutdown = function (dbName, collName, searchQuery, childReadyColl) {
    const testDb = db.getSiblingDB(dbName);

    // Signal the parent that we have connected, so it does not start shutting mongod down before we
    // have a usable connection. This document-insert pattern is used elsewhere (e.g.
    // list_databases_and_rename_collection.js) and avoids the races of a fail point based handshake.
    assert.commandWorked(testDb.getCollection(childReadyColl).insertOne({_id: "child ready"}));

    const fpName = "pauseBeforeShuttingDownSearchTaskExecutor";
    // Wait until shutdown reaches the fail point.
    assert.commandWorked(
        testDb.adminCommand({waitForFailPoint: fpName, timesEntered: 1, maxTimeMS: 60000}),
    );

    try {
        // A $search arriving now must be refused rather than establishing a new cursor against the
        // shutting-down executor.
        const res = testDb.runCommand({
            aggregate: collName,
            pipeline: [{$search: searchQuery}],
            cursor: {},
        });
        assert.commandFailedWithCode(res, ErrorCodes.ShutdownInProgress);
    } finally {
        assert.commandWorked(testDb.adminCommand({configureFailPoint: fpName, mode: "off"}));
    }
};

describe("search executor shutdown", function () {
    it("refuses a new $search while the search executor is shutting down", function () {
        const dbName = jsTestName();
        const collName = jsTestName();

        const mongotmock = new MongotMock();
        mongotmock.start();
        const mongotConn = mongotmock.getConnection();

        const conn = MongoRunner.runMongod({
            setParameter: {
                mongotHost: mongotConn.host,
            },
        });

        const db = conn.getDB(dbName);
        const coll = db[collName];

        assert.commandWorked(coll.insert({_id: 1, title: "cakes"}));

        const searchQuery = {query: "cakes", path: "title"};

        // No mock responses are configured: the $search under test is refused before any command
        // reaches mongot.

        // Pause shutdown so a $search can be attempted in the shutdown window. This must be enabled
        // before shutdown starts, otherwise shutdown could pass the fail point without pausing.
        configureFailPoint(db, "pauseBeforeShuttingDownSearchTaskExecutor");

        const join = startParallelShell(
            funWithArgs(doSearchDuringShutdown, dbName, collName, searchQuery, kChildReadyColl),
            conn.port,
        );

        // Wait until the child has connected (proven by the document it inserts) before shutting
        // mongod down; otherwise shutdown could close the listener before the child connects.
        assert.soon(
            function () {
                return db.getCollection(kChildReadyColl).findOne({_id: "child ready"}) !== null;
            },
            "parallel shell never connected",
            300000,
        );

        const exitCode = MongoRunner.stopMongod(conn);
        join();

        assert.eq(exitCode, MongoRunner.EXIT_CLEAN, "mongod did not shut down cleanly");
        mongotmock.stop();
    });
});
