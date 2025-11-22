/**
 * Tests that the $interruptTest aggregation stage correctly handles interrupts
 * when the checkForInterruptFail failpoint is set.
 *
 * The extension calls checkForInterrupt() in getNext(), which will trigger
 * the failpoint if it's active, causing the operation to be interrupted.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {
    checkPlatformCompatibleWithExtensions,
    generateExtensionConfigs,
    deleteExtensionConfigs,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const kInterruptExtensionUassertCode = 11213400;

// Helper function to get the thread name for a connection.
function getThreadName(db) {
    const myUriRes = assert.commandWorked(db.adminCommand({whatsmyuri: 1}));
    const myUri = myUriRes.you;

    // Get the thread name (connection description) from currentOp.
    // We need an operation to be running to get the thread name, so we'll start a find operation
    // briefly to capture it, or we can get it from a simple command.
    const curOpRes = assert.commandWorked(db.adminCommand({currentOp: 1, client: myUri}));
    let threadName = null;
    if (curOpRes.inprog && curOpRes.inprog.length > 0) {
        threadName = curOpRes.inprog[0].desc;
    } else {
        // If no operation is in progress, we can't get the thread name this way. The failpoint will apply to all threads which suffices for this test.
    }

    return threadName;
}

// Helper function to create a one-time failpoint for a given thread.
function createOneTimeFailpoint(db, threadName) {
    // Configure the failpoint to interrupt operations with 100% chance.
    const failpointData = {chance: 1.0};
    if (threadName) {
        failpointData.threadName = threadName;
    }

    const adminDB = db.getSiblingDB("admin");
    assert.commandWorked(
        adminDB.runCommand({
            configureFailPoint: "checkForInterruptFail",
            mode: {times: 1},
            data: failpointData,
        }),
    );
}

// Main test function that runs all the interrupt tests.
function runInterruptTests(conn) {
    const db = conn.getDB("test");
    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();
    const NUM_DOCS = 50;
    for (var i = 0; i < NUM_DOCS; i++) {
        assert.commandWorked(coll.insert({_id: i}));
    }

    // Set the checkForInterruptFail failpoint and verify the query gets interrupted.
    // Get the thread name for the connection that will run the query.
    // The failpoint needs the thread description (desc) from currentOp, not the URI.
    const threadName = getThreadName(db);

    // Sanity check - verify that without a fail point, the query throws a uassert.
    (function interruptExtensionThrowsWithNoInterrupt() {
        assert.throwsWithCode(
            () => coll.aggregate([{$sort: {_id: 1}}, {$interruptTest: {}}]),
            kInterruptExtensionUassertCode,
        );
    })();

    // Test that when the failpoint is set before the query, the query checks for interrupt and exits without throwing the custom uassert.
    (function interruptExtensionChecksForInterrupt() {
        createOneTimeFailpoint(db, threadName);

        // Run the aggregation with the failpoint active.
        // This should trigger an interrupt when checkForInterrupt is called in getNext(), and we should not uassert in $interruptTest. Instead, we should receive the interrupted kill code.
        assert.throwsWithCode(() => coll.aggregate([{$sort: {_id: 1}}, {$interruptTest: {}}]), ErrorCodes.Interrupted);
    })();

    // Sanity checks that without the interrupt, the query uasserts after a certain number of calls using getMore.
    (function interruptExtensionWithGetMoreFails() {
        // This query will uassert on the `uassertOn`th doc.
        const uassertOn = 10;
        const initialResult = db.runCommand({
            aggregate: collName,
            pipeline: [{$sort: {_id: 1}}, {$interruptTest: {uassertOn}}],
            cursor: {batchSize: 1},
        });
        let cursorId = initialResult.cursor.id;

        // We have already run getNext() once, so advance up to the `uassertOn`th doc.
        for (var i = 1; i < uassertOn; i++) {
            assert.commandWorked(db.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
        }

        // Calling getMore one more time should result in the custom error code.
        assert.commandFailedWithCode(
            db.runCommand({getMore: cursorId, collection: collName, batchSize: 1}),
            kInterruptExtensionUassertCode,
        );
    })();

    // Test that setting a fail point right before the uassert iteration throws with the interrupted error code instead.
    (function interruptExtensionWithGetMoreHitsInterruptCheck() {
        // This query will uassert on the `uassertOn`th doc.
        const uassertOn = 10;
        const initialResult = db.runCommand({
            aggregate: collName,
            pipeline: [{$sort: {_id: 1}}, {$interruptTest: {uassertOn}}],
            cursor: {batchSize: 1},
        });
        let cursorId = initialResult.cursor.id;

        // We have already run getNext() once, so advance up to the `uassertOn`th doc.
        for (var i = 1; i < uassertOn; i++) {
            assert.commandWorked(db.runCommand({getMore: cursorId, collection: collName, batchSize: 1}));
        }

        createOneTimeFailpoint(db, threadName);

        // Calling getMore one more time should result in the interrupted error code.
        assert.commandFailedWithCode(
            db.runCommand({getMore: cursorId, collection: collName, batchSize: 1}),
            ErrorCodes.Interrupted,
        );
    })();
}

const extensionNames = generateExtensionConfigs("libinterrupt_mongo_extension.so");

try {
    const conn = MongoRunner.runMongod({
        loadExtensions: extensionNames[0],
    });
    assert.neq(null, conn, "failed to start mongod");

    runInterruptTests(conn);

    MongoRunner.stopMongod(conn);
} finally {
    deleteExtensionConfigs(extensionNames);
}
