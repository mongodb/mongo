/**
 * Confirms that index builds can be aborted using killop.
 */
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const testDB = conn.getDB("test");
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.test.insert({a: 1}));
const coll = testDB.test;

// Test that building an index can be aborted using killop.
function testAbortIndexBuild() {
    IndexBuildTest.pauseIndexBuilds(conn);

    const createIdx = IndexBuildTest.startIndexBuild(conn, coll.getFullName(), {a: 1});

    // When the index build starts, find its op id.
    const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

    // Kill the index build.
    assert.commandWorked(testDB.killOp(opId));

    // Wait for the index build to stop.
    try {
        IndexBuildTest.waitForIndexBuildToStop(testDB);
    } finally {
        IndexBuildTest.resumeIndexBuilds(conn);
    }

    const exitCode = createIdx({checkExitSuccess: false});
    assert.neq(
        0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

    // Check that no new index has been created.  This verifies that the index build was aborted
    // rather than successfully completed.
    IndexBuildTest.assertIndexes(coll, 1, ['_id_']);
}

testAbortIndexBuild();
MongoRunner.stopMongod(conn);