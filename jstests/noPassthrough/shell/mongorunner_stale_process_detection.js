/**
 * Regression test for the stale-process misconnection bug in MongoRunner.awaitConnection.
 *
 * In the real failure mode, a previous test left a mongod running and a new test tried to start
 * a mongod on the same port. The new process exits immediately with EXIT_NET_ERROR (48, "address
 * already in use"). Before the fix, awaitConnection would TCP-connect to the already-running
 * process, see a successful connection, and return it — silently handing the caller a connection
 * to the wrong mongod. The caller's subsequent operations would hit that process's (possibly
 * deleted) dbpath and produce confusing NoSuchKey / ENOENT errors.
 *
 * After the fix, awaitConnection verifies that serverStatus.pid matches the pid it started and
 * throws StopError(EXIT_NET_ERROR) when there is a mismatch.
 *
 * We reproduce the pid-mismatch condition directly: start two mongods, then ask awaitConnection
 * to verify mongod2's pid against mongod1's port. The server that answers has mongod1's pid,
 * triggering the same guard that fires in the real stale-process scenario.
 */

import {after, describe, it} from "jstests/libs/mochalite.js";

describe("MongoRunner.awaitConnection stale-process detection", function () {
    let mongod1, mongod2;

    after(function () {
        if (mongod2) MongoRunner.stopMongod(mongod2);
        if (mongod1)
            // awaitConnection({pid: mongod2.pid, port: mongod1.port}) set serverExitCodeMap for
            // mongod1's port to EXIT_NET_ERROR, so stopMongod would not actually send SIGTERM.
            // Use stopMongoProgramByPid to kill the process directly instead.
            stopMongoProgramByPid(+mongod1.pid);
    });

    it("throws StopError(EXIT_NET_ERROR) when server pid does not match the expected pid", function () {
        mongod1 = MongoRunner.runMongod();
        mongod2 = MongoRunner.runMongod();

        // Intentionally pass mongod2's pid with mongod1's port. The process that answers on
        // mongod1's port has mongod1's pid, so awaitConnection sees a mismatch and should
        // throw StopError — the same path that fires when a stale process occupies the port.
        let caughtError;
        try {
            MongoRunner.awaitConnection({pid: mongod2.pid, port: mongod1.port});
        } catch (e) {
            caughtError = e;
        }

        assert(caughtError instanceof MongoRunner.StopError, "Expected MongoRunner.StopError", {caughtError});
        assert.eq(MongoRunner.EXIT_NET_ERROR, caughtError.returnCode, "Expected EXIT_NET_ERROR (48)", {caughtError});
    });
});
