/**
 * Tests a data race between setParameter of
 * replWriterThreadCount / replWriterMinThreadCount and replication shutdown.
 *
 * Previously, _workerPool could be moved/destroyed concurrently with setParameter.
 * If the bug is present, this race is detected deterministically on TSAN builds.
 *
 * @tags: [requires_replication]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("replWriterThreadCount setParameter vs shutdown race", function () {
    it("does not race when setParameter runs concurrently with SIGTERM", function () {
        const rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();

        const primary = rst.getPrimary();
        const testDB = primary.getDB("test");

        // Each parallel shell writes a document to signal that it has connected and is
        // actively sending setParameter commands. The main thread waits for all
        // signals before triggering shutdown.
        assert.commandWorked(testDB.createCollection("ready"));

        const kNumShells = 8;
        const shells = [];
        for (let i = 0; i < kNumShells; i++) {
            const shell = startParallelShell(
                funWithArgs(function (shellIdx) {
                    // Signal that this shell has connected.
                    db.getSiblingDB("test").ready.insert({_id: shellIdx});

                    while (true) {
                        try {
                            const maxVal = Math.random() > 0.5 ? 16 : 32;
                            assert.commandWorked(
                                db.adminCommand({setParameter: 1, replWriterThreadCount: maxVal}),
                            );
                            assert.commandWorked(
                                db.adminCommand({setParameter: 1, replWriterMinThreadCount: 0}),
                            );
                        } catch (e) {
                            // Network error when the server shuts down.
                            break;
                        }
                    }
                }, i),
                primary.port,
            );
            shells.push(shell);
        }

        // Wait until all parallel shells have connected and started sending
        // commands.
        assert.soon(
            () => testDB.ready.count() >= kNumShells,
            "Timed out waiting for parallel shells to connect",
            120000,
            100,
        );

        // stopSet() sends SIGTERM, which triggers
        // ReplicationCoordinatorExternalStateImpl::shutdown ->
        // _stopDataReplication.
        rst.stopSet();

        // Join the parallel shells. They will have exited (with errors) once the
        // server went away.
        for (const shell of shells) {
            try {
                shell();
            } catch (e) {
                // Expected: shells exit non-zero when their connection drops.
            }
        }
    });
});
