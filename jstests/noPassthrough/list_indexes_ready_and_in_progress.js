/**
 * Tests that the listIndexes command's default is to only show ready indexes; and that
 * the 'includeIndexBuilds' flag can be set to include indexes that are still building
 * along with the ready indexes.
 */
(function() {
    "use strict";

    load("jstests/noPassthrough/libs/index_build.js");

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("test");
    assert.commandWorked(testDB.dropDatabase());

    let coll = testDB.list_indexes_ready_and_in_progress;
    coll.drop();
    assert.commandWorked(testDB.createCollection(coll.getName()));
    IndexBuildTest.assertIndexes(coll, 1, ["_id_"]);
    assert.commandWorked(coll.createIndex({a: 1}));
    IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

    IndexBuildTest.pauseIndexBuilds(conn);
    const createIdx =
        IndexBuildTest.startIndexBuild(conn, coll.getFullName(), {b: 1}, {background: true});
    IndexBuildTest.waitForIndexBuildToStart(testDB);

    // Verify there is no third index.
    IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

    // The listIndexes command supports returning all indexes, including ones that are not ready.
    IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1"], ["b_1"], {includeIndexBuilds: true});

    IndexBuildTest.resumeIndexBuilds(conn);

    // Wait for the index build to stop.
    IndexBuildTest.waitForIndexBuildToStop(testDB);

    const exitCode = createIdx();
    assert.eq(0, exitCode, 'expected shell to exit cleanly');

    IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1", "b_1"]);
    MongoRunner.stopMongod(conn);
}());
