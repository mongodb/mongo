(function() {
"use strict";

load("jstests/libs/parallelTester.js");

const conn = MongoRunner.runMongod();
assert.neq(null, conn);
const kDbName = "test_failcommand_noparallel";
const db = conn.getDB(kDbName);

// Test times when closing connection.
// Use distinct because it is rarely used by internal operations, making it less likely unrelated
// activity triggers the failpoint.
assert.commandWorked(db.adminCommand({
    configureFailPoint: "failCommand",
    mode: {times: 2},
    data: {
        closeConnection: true,
        failCommands: ["distinct"],
    }
}));
assert.throws(() => db.runCommand({distinct: "c", key: "_id"}));
assert.throws(() => db.runCommand({distinct: "c", key: "_id"}));
assert.commandWorked(db.runCommand({distinct: "c", key: "_id"}));
assert.commandWorked(db.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test the blockConnection patterns.
jsTest.log("Test validation of blockConnection fields");
{
    // 'blockTimeMS' is required when 'blockConnection' is true.
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            blockConnection: true,
            failCommands: ["hello"],
        }
    }));
    assert.commandFailedWithCode(db.runCommand({hello: 1}), ErrorCodes.InvalidOptions);

    // 'blockTimeMS' must be non-negative.
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            blockConnection: true,
            blockTimeMS: -100,
            failCommands: ["hello"],
        }
    }));
    assert.commandFailedWithCode(db.runCommand({hello: 1}), ErrorCodes.InvalidOptions);

    assert.commandWorked(db.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
}

// Insert a test document.
assert.commandWorked(db.runCommand({
    insert: "c",
    documents: [{_id: 'block_test', run_id: 0}],
}));

/**
 * Returns the "run_id" of the test document.
 */
function checkRunId() {
    const ret = db.runCommand({find: "c", filter: {_id: 'block_test'}});
    assert.commandWorked(ret);

    const doc = ret["cursor"]["firstBatch"][0];
    return doc["run_id"];
}

/**
 * Runs update to increment the "run_id" of the test document by one.
 */
function incrementRunId() {
    assert.commandWorked(db.runCommand({
        update: "c",
        updates: [{q: {_id: 'block_test'}, u: {$inc: {run_id: 1}}}],
    }));
}

/**
 * Starts and returns a thread for updating the test document by incrementing the
 * "run_id" by one.
 */
function startIncrementRunIdThread() {
    const latch = new CountDownLatch(1);
    let thread = new Thread(function(connStr, dbName, latch) {
        jsTest.log("Sending update");

        const client = new Mongo(connStr);
        const db = client.getDB(dbName);
        latch.countDown();
        assert.commandWorked(db.runCommand({
            update: "c",
            updates: [{q: {_id: 'block_test'}, u: {$inc: {run_id: 1}}}],
        }));

        jsTest.log("Successfully applied update");
    }, conn.name, kDbName, latch);
    thread.start();
    latch.await();
    return thread;
}

assert.eq(checkRunId(), 0);
const kLargeBlockTimeMS = 60 * 1000;

jsTest.log("Test that only commands listed in failCommands block");
{
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            blockConnection: true,
            blockTimeMS: kLargeBlockTimeMS,
            failCommands: ["update"],
        }
    }));
    let thread = startIncrementRunIdThread();

    // Check that other commands get through.
    assert.commandWorked(db.runCommand({hello: 1}));
    assert.eq(checkRunId(), 0);

    // Wait for the blocked update to get through.
    thread.join();
    assert.soon(() => {
        return checkRunId() == 1;
    });

    assert.commandWorked(db.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
}

jsTest.log("Test command changes");
{
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            blockConnection: true,
            blockTimeMS: kLargeBlockTimeMS,
            failCommands: ["update", "insert"],
        }
    }));
    assert.eq(checkRunId(), 1);

    // Drop update from the command list and verify that the update gets through.
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            blockConnection: true,
            blockTimeMS: kLargeBlockTimeMS,
            failCommands: ["insert"],
        }
    }));
    incrementRunId();
    assert.eq(checkRunId(), 2);

    assert.commandWorked(db.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
}

MongoRunner.stopMongod(conn);
}());
