/**
 * Simulates a race between shutdown (where the replication worker pool is shut down) and a
 * concurrent setParameter for replWriterMinThreadCount.
 *
 * This is a regression test for SERVER-136903.
 * Previously, there was a bug where ThreadPool::setMinThreads() could hang forever
 * if the pool is not running, and the new minimum thread count exceeds the current thread count.
 *
 * Tests two scenarios:
 *
 *  1. If setParameter runs after _workerPool->join(): setParameter itself never returns.
 *  2. If setParameter runs between _workerPool->shutdown() and _workerPool->join(): setParameter spins while holding the
 *     pool mutex, so join() can never acquire it and server shutdown never completes.
 *
 * If fixed, setParameter returns promptly in both cases and the server exits cleanly.
 *
 * @tags: [requires_replication]
 */
import {describe, it, afterEach} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const kAfterJoinFP = "hangAfterWorkerPoolJoinInStopDataReplication";
const kBeforeJoinFP = "hangAfterWorkerPoolShutdownBeforeJoinInStopDataReplication";

describe("replWriterMinThreadCount setParameter vs shutdown hang", function () {
    let rst;

    afterEach(function () {
        if (rst) {
            rst.stopSet();
            rst = null;
        }
    });

    function setup(failPoint) {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        const primary = rst.getPrimary();
        const dbName = jsTestName();
        const testDB = primary.getDB(dbName);

        assert.commandWorked(
            primary.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}),
        );

        // Parallel shell that sends setParameter once shutdown has reached the failpoint. It
        // connects now, before shutdown stops accepting new sessions.
        const setParamShell = startParallelShell(
            funWithArgs(
                function (dbName, failPoint) {
                    // Signal that this shell has a live connection.
                    db.getSiblingDB(dbName).ready.insert({_id: "setparam"});

                    assert.commandWorked(
                        db.adminCommand({
                            waitForFailPoint: failPoint,
                            timesEntered: 1,
                            maxTimeMS: 120000,
                        }),
                    );

                    // If the bug is present, this never returns.
                    try {
                        assert.commandWorked(
                            db.adminCommand({setParameter: 1, replWriterMinThreadCount: 1}),
                        );
                    } catch (e) {
                        // Network error during shutdown is acceptable.
                    }

                    // Only reached if the bug is fixed.
                    try {
                        db.getSiblingDB(dbName).signal.insert({_id: "done"});
                    } catch (e) {
                        // Write may fail if the server is shutting down.
                    }
                },
                dbName,
                failPoint,
            ),
            primary.port,
        );

        assert.soon(
            () => testDB.ready.find({_id: "setparam"}).count() > 0,
            "Timed out waiting for parallel shell to connect",
            60000,
            500,
        );

        const shutdownShell = startParallelShell(function () {
            try {
                db.adminCommand({shutdown: 1, force: true});
            } catch (e) {
                // Expected: connection drops when the server shuts down.
            }
        }, primary.port);

        assert.commandWorked(
            primary.adminCommand({waitForFailPoint: failPoint, timesEntered: 1, maxTimeMS: 120000}),
        );

        return {primary, testDB, setParamShell, shutdownShell};
    }

    function joinShells(shells) {
        for (const shell of shells) {
            try {
                shell();
            } catch (e) {
                // Expected: network errors during shutdown.
            }
        }
    }

    it("setParameter does not hang when the pool is already joined", function () {
        const {primary, testDB, setParamShell, shutdownShell} = setup(kAfterJoinFP);

        // If the bug is present, setParameter never returns and this signal is never written.
        assert.soon(
            () => {
                try {
                    return testDB.signal.find({_id: "done"}).count() > 0;
                } catch (e) {
                    return false;
                }
            },
            "setParameter replWriterMinThreadCount hung after worker pool join",
            30000,
            1000,
            {runHangAnalyzer: false},
        );

        assert.commandWorked(primary.adminCommand({configureFailPoint: kAfterJoinFP, mode: "off"}));
        joinShells([shutdownShell, setParamShell]);
    });

    it("shutdown completes when setParameter races with pool join", function () {
        const {primary, setParamShell, shutdownShell} = setup(kBeforeJoinFP);
        const pid = primary.pid;

        // Wait until setMinThreads has been entered on the repl worker pool. With the bug it is
        // now spinning while holding the pool mutex.
        assert.soon(
            () =>
                checkLog.checkContainsOnceJson(primary, 11280001, {
                    poolName: "ReplWriterWorkerThreadPool",
                }),
            "setMinThreads was never invoked on the repl worker pool",
            60000,
            500,
            {runHangAnalyzer: false},
        );

        // Let _stopDataReplication proceed to _workerPool->join(). With the bug, join() blocks waiting on
        // the mutex held by the spinning setMinThreads and the process never exits.
        assert.commandWorked(
            primary.adminCommand({configureFailPoint: kBeforeJoinFP, mode: "off"}),
        );

        assert.soon(
            () => !checkProgram(pid).alive,
            "mongod did not exit: shutdown hung in _workerPool->join() behind setMinThreads",
            60000,
            1000,
            {runHangAnalyzer: false},
        );

        joinShells([shutdownShell, setParamShell]);
    });
});
