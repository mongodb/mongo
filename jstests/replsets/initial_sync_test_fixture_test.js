/**
 * Test to check that the Initial Sync Test Fixture properly pauses initial sync.
 *
 * The test checks that both the collection cloning and oplog application stages of initial sync
 * pause after exactly one command is run when the test fixture's step function is called. The test
 * issues the same listDatabases and listCollections commands that collection cloning does so we
 * know all the commands that will be run on the sync source and can verify that only one is run per
 * call to step(). Similarly for oplog application, we can check the log messages to make sure that
 * the batches being applied are of the expected size and that only one batch was applied per step()
 * call.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/replsets/libs/initial_sync_test.js");

/**
 * Helper function to check that specific messages appeared or did not appear in the logs.
 */
function checkLogForMsg(node, msg, contains) {
    if (contains) {
        jsTest.log("Check for presence of message (" + node.port + "): |" + msg + "|");
        assert(checkLog.checkContainsOnce(node, msg));
    } else {
        jsTest.log("Check for absence of message (" + node.port + "): |" + msg + "|");
        assert(!checkLog.checkContainsOnce(node, msg));
    }
}

/**
 * Helper function to check that specific messages appeared or did not appear in the logs. If we
 * expect the log message to appear, this will show that the node is paused after getting the
 * specified timestamp.
 */
function checkLogForGetTimestampMsg(node, timestampName, timestamp, contains) {
    let msg = "Initial Syncer got the " + timestampName + ": { ts: " + tojson(timestamp);

    checkLogForMsg(node, msg, contains);
}

/**
 * Helper function to check that specific messages appeared or did not appear in the logs. If
 * the command was listIndexes and we expect the message to appear, we also add the collection
 * UUID to make sure that it corresponds to the expected collection.
 */
function checkLogForCollectionClonerMsg(node, commandName, dbname, contains, collUUID) {
    let msg = 'Collection Cloner scheduled a remote command","attr":{"stage":"' + dbname +
        " db: { " + commandName;

    if (commandName === "listIndexes" && contains) {
        msg += ": " + collUUID;
        msg = msg.replace('("', '(\\"').replace('")', '\\")');
    }

    checkLogForMsg(node, msg, contains);
}

/**
 * Helper function to check that the specific message appeared exactly once in the logs and that
 * there is no other message saying that the next batch is about to be applied. This will show
 * that oplog application is paused.
 */
function checkLogForOplogApplicationMsg(node, size) {
    let msg = "Initial Syncer is about to apply the next oplog batch of size: ";
    checkLog.containsWithCount(node, msg, 1, 5 * 1000);

    msg += size;
    assert(checkLog.checkContainsOnce(node, msg));
}

// Set up Initial Sync Test.
const rst = new ReplSetTest({
    name: "InitialSyncTest",
    nodes: [
        {
            // Each PrimaryOnlyService rebuilds its instances on stepup, and that may involve
            // doing writes. So we need to disable PrimaryOnlyService rebuild to make the number
            // of oplog batches check below work reliably.
            setParameter: {
                "failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"}),
            }
        },
        {rsConfig: {priority: 0, votes: 0}}
    ]
});
rst.startSet();
rst.initiate();

const initialSyncTest = new InitialSyncTest("InitialSyncTest", rst);
try {
    // If the test fails, the initial syncing node may be left with an engaged failpoint that
    // instructs it to hang. This `try` block is to guarantee we call `initialSyncTest.fail()` which
    // allows the test to gracefully exit with an error.

    const primary = initialSyncTest.getPrimary();
    let secondary = initialSyncTest.getSecondary();
    const db = primary.getDB("test");
    let maxLargeStringsInBatch = 9;
    // If we can fit exactly 9+1=10 large strings in a batch, the small overhead for each oplog
    // entry means we expect only 9 oplog entries per batch.
    let largeStringSize = initialSyncTest.replBatchLimitBytes / (maxLargeStringsInBatch + 1);
    const largeString = 'z'.repeat(largeStringSize);

    assert.commandWorked(db.foo.insert({a: 1}));
    assert.commandWorked(db.bar.insert({b: 1}));

    // Prepare a transaction so that we know that step() can restart the secondary even if there is
    // a prepared transaction. The prepareTimestamp will be used as the beginFetchingTimestamp and
    // beginApplyingTimestamp during initial sync.
    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.getCollection("foo");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({c: 1}));
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // This step call restarts the secondary and causes it to go into initial sync. It will pause
    // initial sync after the node has fetched the defaultBeginFetchingTimestamp.
    assert(!initialSyncTest.step());

    secondary = initialSyncTest.getSecondary();
    secondary.setSecondaryOk();

    // Make sure that we cannot read from this node yet.
    assert.commandFailedWithCode(secondary.getDB("test").runCommand({count: "foo"}),
                                 ErrorCodes.NotPrimaryOrSecondary);

    // Make sure that we see that the node got the defaultBeginFetchingTimestamp, but hasn't gotten
    // the beginFetchingTimestamp yet.
    checkLogForGetTimestampMsg(secondary, "defaultBeginFetchingTimestamp", prepareTimestamp, true);
    checkLogForGetTimestampMsg(secondary, "beginFetchingTimestamp", prepareTimestamp, false);
    checkLogForGetTimestampMsg(secondary, "beginApplyingTimestamp", prepareTimestamp, false);

    // This step call will resume initial sync and pause it again after the node gets the
    // beginFetchingTimestamp from its sync source.
    assert(!initialSyncTest.step());

    // Make sure that we cannot read from this node yet.
    assert.commandFailedWithCode(secondary.getDB("test").runCommand({count: "foo"}),
                                 ErrorCodes.NotPrimaryOrSecondary);

    // Make sure that we see that the node got the beginFetchingTimestamp, but hasn't gotten the
    // beginApplyingTimestamp yet.
    checkLogForGetTimestampMsg(secondary, "defaultBeginFetchingTimestamp", prepareTimestamp, false);
    checkLogForGetTimestampMsg(secondary, "beginFetchingTimestamp", prepareTimestamp, true);
    checkLogForGetTimestampMsg(secondary, "beginApplyingTimestamp", prepareTimestamp, false);

    // This step call will resume initial sync and pause it again after the node gets the
    // beginApplyingTimestamp from its sync source.
    assert(!initialSyncTest.step());

    // Make sure that we cannot read from this node yet.
    assert.commandFailedWithCode(secondary.getDB("test").runCommand({count: "foo"}),
                                 ErrorCodes.NotPrimaryOrSecondary);

    // Make sure that we see that the node got the beginApplyingTimestamp, but that we don't see the
    // listDatabases call yet.
    checkLogForGetTimestampMsg(secondary, "defaultBeginFetchingTimestamp", prepareTimestamp, false);
    checkLogForGetTimestampMsg(secondary, "beginFetchingTimestamp", prepareTimestamp, false);
    checkLogForGetTimestampMsg(secondary, "beginApplyingTimestamp", prepareTimestamp, true);
    checkLogForCollectionClonerMsg(secondary, "listDatabases", "admin", false);

    // This step call will resume initial sync and pause it again after the node gets the
    // listDatabases result from its sync source.
    assert(!initialSyncTest.step());

    // Make sure that we cannot read from this node yet.
    assert.commandFailedWithCode(secondary.getDB("test").runCommand({count: "foo"}),
                                 ErrorCodes.NotPrimaryOrSecondary);

    // Make sure that we saw the listDatabases call in the log messages, but didn't see any
    // listCollections or listIndexes call.
    checkLogForCollectionClonerMsg(secondary, "listDatabases", "admin", true);
    checkLogForCollectionClonerMsg(secondary, "listCollections", "admin", false);
    checkLogForCollectionClonerMsg(secondary, "listIndexes", "admin", false);

    // Do same listDatabases command as CollectionCloner.
    const databases =
        assert.commandWorked(primary.adminCommand({listDatabases: 1, nameOnly: true})).databases;

    // Iterate over the databases and collections in the same order that the test fixture would so
    // that we can check the log messages to make sure initial sync is paused as expected.
    for (let dbObj of databases) {
        const dbname = dbObj.name;

        // We skip the local db during the collection cloning phase of initial sync.
        if (dbname === "local") {
            continue;
        }

        const database = primary.getDB(dbname);

        // Do same listCollections command as CollectionCloner.
        const res = assert.commandWorked(database.runCommand(
            {listCollections: 1, filter: {$or: [{type: "collection"}, {type: {$exists: false}}]}}));

        // Make sure that there is only one batch.
        assert.eq(NumberLong(0), res.cursor.id, res);

        const collectionsCursor = res.cursor;

        // For each database, CollectionCloner will first call listCollections.
        assert(!initialSyncTest.step());

        // Make sure that we cannot read from this node yet.
        assert.commandFailedWithCode(secondary.getDB("test").runCommand({count: "foo"}),
                                     ErrorCodes.NotPrimaryOrSecondary);

        // Make sure that we saw the listCollections call in the log messages, but didn't see a
        // listIndexes call.
        checkLogForCollectionClonerMsg(secondary, "listCollections", dbname, true);
        checkLogForCollectionClonerMsg(secondary, "listIndexes", "admin", false);

        for (let collectionObj of collectionsCursor.firstBatch) {
            assert(collectionObj.info, collectionObj);
            const collUUID = collectionObj.info.uuid;

            // For each collection, CollectionCloner will call listIndexes.
            assert(!initialSyncTest.step());

            // Make sure that we cannot read from this node yet.
            assert.commandFailedWithCode(secondary.getDB("test").runCommand({count: "foo"}),
                                         ErrorCodes.NotPrimaryOrSecondary);

            // Make sure that we saw the listIndexes call in the log messages, but didn't
            // see a listCollections call.
            checkLogForCollectionClonerMsg(secondary, "listIndexes", dbname, true, collUUID);
            checkLogForCollectionClonerMsg(secondary, "listCollections", "admin", false);
        }
    }

    // Perform large operations during collection cloning so that we will need multiple batches
    // during oplog application. For simplicity, guarantee we will create only two batches during
    // the oplog application phase of initial sync.
    const docsToInsertPerCollectionDuringOplogApplication = maxLargeStringsInBatch - 1;
    const totalDocsInserted = 2 * docsToInsertPerCollectionDuringOplogApplication;
    for (let count = 0; count < docsToInsertPerCollectionDuringOplogApplication; ++count) {
        assert.commandWorked(db.foo.insert({d: largeString}));
        assert.commandWorked(db.bar.insert({e: largeString}));
    }

    // Check that we see the expected number of batches during oplog application.

    // This batch should correspond to the 'prepare' op.
    assert(!initialSyncTest.step());
    checkLogForOplogApplicationMsg(secondary, 1);
    assert(!initialSyncTest.step());
    checkLogForOplogApplicationMsg(secondary, maxLargeStringsInBatch);
    assert(!initialSyncTest.step());
    checkLogForOplogApplicationMsg(secondary, totalDocsInserted - maxLargeStringsInBatch);

    assert(initialSyncTest.step(), "Expected initial sync to have completed, but it did not");

    // Abort transaction so that the data consistency checks in stop() can run.
    assert.commandWorked(session.abortTransaction_forTesting());

    // Issue a w:2 write to make sure the secondary has replicated the abortTransaction oplog entry.
    assert.commandWorked(primary.getDB("otherDB").otherColl.insert({x: 1}, {writeConcern: {w: 2}}));

    // Confirm that node can be read from and that it has the inserts that were made while the node
    // was in initial sync. We inserted `docsToInsertPerCollectionDuringOplogApplication` + 1
    // additional document prior to the oplog application phase to each of `foo` and `bar`.
    assert.eq(secondary.getDB("test").foo.find().count(),
              docsToInsertPerCollectionDuringOplogApplication + 1);
    assert.eq(secondary.getDB("test").bar.find().count(),
              docsToInsertPerCollectionDuringOplogApplication + 1);
    assert.eq(secondary.getDB("test").foo.find().itcount(),
              docsToInsertPerCollectionDuringOplogApplication + 1);
    assert.eq(secondary.getDB("test").bar.find().itcount(),
              docsToInsertPerCollectionDuringOplogApplication + 1);

    // Do data consistency checks at the end.
    initialSyncTest.stop();
} catch (errorDuringTest) {
    initialSyncTest.fail();
    throw errorDuringTest;
}
})();
