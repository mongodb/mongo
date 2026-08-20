/**
 * Test that the --shutdown option of mongod successfully shuts down another running node.
 */

import {isLinux} from "jstests/libs/server_security/os_helpers.js";

if (!isLinux()) {
    jsTestLog("Skipping test due to non-linux platform.");
    quit();
}

const conn = MongoRunner.runMongod();

// This should shutdown the mongod we just started and exit cleanly. runMongod will throw
// if we exit with a non-zero exit code.
const shutdownConn = MongoRunner.runMongod({dbpath: conn.dbpath, shutdown: "", noCleanData: true});

// Check that the process we shut down exited cleanly.
const exitCode = waitMongoProgram(conn.port);
assert.eq(exitCode, MongoRunner.EXIT_CLEAN);
