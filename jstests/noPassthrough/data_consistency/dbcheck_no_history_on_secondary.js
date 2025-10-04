/**
 * Confirms that a dbCheck batch operation logs an error in the health log of a secondary that does
 * not have enough available history.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({
    name: "dbcheck_lag",
    nodes: 2,
    nodeOptions: {
        setParameter: {
            // Set the history window to ensure that dbCheck does not cause the server to crash
            // even when no history is available.
            minSnapshotHistoryWindowInSeconds: 0,
        },
    },
});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const testDB = primary.getDB("test");
let docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({a: i});
}
assert.commandWorked(testDB.foo.insert(docs));

const sleepMs = 3000;
const fp = configureFailPoint(primary, "SleepDbCheckInBatch", {sleepMs: sleepMs});

// Returns immediately and starts a background task.
assert.commandWorked(testDB.getSiblingDB("test").runCommand({dbCheck: 1}));

// Wait for dbCheck hasher to acquire snapshot.
fp.wait();

// Write some data to advance the durable timestamp while we're waiting for dbCheck to run.
docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({_id: i});
}
assert.commandWorked(testDB.foo.insert(docs));

fp.off();

// Wait for primary to complete the task and the secondaries to process. Note that we still have to
// wait for the health log entries to appear because they are logged shortly after processing
// batches.
const dbCheckCompleted = (db) => {
    return db.currentOp().inprog.filter((x) => x["desc"] == "dbCheck")[0] === undefined;
};
assert.soon(() => dbCheckCompleted(testDB), "dbCheck timed out");
replTest.awaitReplication();

{
    // Expect no errors on the primary. Health log write is logged after batch is replicated.
    const healthlog = primary.getDB("local").system.healthlog;
    assert.soon(() => healthlog.find().hasNext(), "expected health log to not be empty");

    const errors = healthlog.find({severity: "error"});
    assert(!errors.hasNext(), () => "expected no errors, found: " + tojson(errors.next()));
}

{
    // Expect an error on the secondary.
    const healthlog = secondary.getDB("local").system.healthlog;
    assert.soon(
        () => healthlog.find({severity: "error"}).hasNext(),
        "expected health log to have an error, but found none",
    );

    const errors = healthlog.find({severity: "error"});
    assert(errors.hasNext(), "expected error, found none");

    const error = errors.next();
    assert.eq(false, error.data.success, "expected failure, found success. log entry: " + tojson(error));
    assert(
        error.data.error.includes("SnapshotTooOld"),
        "expected to find SnapshotTooOld error. log entry: " + tojson(error),
    );
}

replTest.stopSet();
