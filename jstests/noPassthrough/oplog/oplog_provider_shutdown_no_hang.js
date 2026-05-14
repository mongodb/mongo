/**
 * Regression test for SERVER-125455: "Hang stopping OplogProvider and subsequent total mongod hang".
 *
 * The bug: OplogProvider::stop() invokes scopedExecutor->shutdown() followed by
 * scopedExecutor->join(), then destructs the held OperationContext. If a consumer task
 * scheduled on that executor is parked in an uninterruptible resource wait (the original
 * report points at WT eviction under WiredTigerRecoveryUnit::_txnOpen), the only thing that
 * could unblock it is marking its OperationContext as killed -- and that doesn't happen
 * until after the join returns. join() waits forever, the OplogProvider thread never
 * exits, and any subsequent shutdown command on mongod hangs in turn.
 *
 * This test:
 *   1. Starts a single-node replica set so the oplog and any oplog-provider machinery
 *      are exercised end-to-end.
 *   2. Drives enough oplog activity to keep the provider's consumer paths warm during
 *      the shutdown window.
 *   3. Sends SIGTERM (MongoRunner.stopMongod's default) and asserts the process exits
 *      within 30 seconds with a clean exit code. A hang would either blow past the
 *      bound or escalate to SIGKILL and produce a non-zero exit code.
 *
 * The bound matches the timeout downstream operators use when triaging a stuck node;
 * anything north of 30s is the failure mode SERVER-125455 describes.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const SHUTDOWN_BUDGET_MS = 30 * 1000;
const SIGTERM = 15;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("oplog_provider_shutdown_no_hang");
const coll = db.getCollection("c");

// Seed the oplog so any provider-side warm-up paths have material to operate on.
const seedBulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 200; i++) {
    seedBulk.insert({_id: i, payload: "x".repeat(64)});
}
assert.commandWorked(seedBulk.execute());

// Background writers keep oplog activity flowing right up until shutdown. They're best
// effort: the parallel shell will fail at shutdown time, which is expected.
const writers = [];
for (let w = 0; w < 4; w++) {
    writers.push(
        startParallelShell(
            `
            const db = db.getSiblingDB("oplog_provider_shutdown_no_hang");
            const coll = db.getCollection("c");
            let i = 1000 + ${w} * 100000;
            try {
                while (true) {
                    assert.commandWorked(coll.insert({_id: i++, w: ${w}, payload: "y".repeat(96)}));
                }
            } catch (e) {
                // The mongod is shutting down. Swallow the connection error and exit.
            }
        `,
            primary.port,
        ),
    );
}

// Give the writers a moment to actually take effect.
sleep(2000);

jsTest.log.info("Sending SIGTERM to primary; expecting a clean exit within " + SHUTDOWN_BUDGET_MS + "ms.");
const startMs = Date.now();
const exitCode = MongoRunner.stopMongod(primary, SIGTERM, {waitpid: true});
const elapsedMs = Date.now() - startMs;
jsTest.log.info("mongod exited with code " + exitCode + " after " + elapsedMs + "ms.");

assert.eq(
    MongoRunner.EXIT_CLEAN,
    exitCode,
    "mongod did not shut down cleanly on SIGTERM (got " + exitCode + "); SERVER-125455 regression",
);
assert.lt(
    elapsedMs,
    SHUTDOWN_BUDGET_MS,
    "mongod took " + elapsedMs + "ms to shut down on SIGTERM, exceeding the " + SHUTDOWN_BUDGET_MS +
        "ms budget; SERVER-125455 regression (OplogProvider::stop deadlock)",
);

// Let the parallel shells unwind. They are expected to fail because the server is gone.
for (const awaitWriter of writers) {
    awaitWriter({checkExitSuccess: false});
}

// stopSet here is a no-op for the already-stopped node but keeps the harness happy.
rst.stopSet();
