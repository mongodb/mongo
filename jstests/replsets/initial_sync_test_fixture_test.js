/*
 * Test to check that the Initial Sync Test Fixture properly pauses initial sync.
 *
 * The test checks that both the collection cloning and oplog application stages of initial sync
 * pause after exactly one commad is run when the test fixture's step function is called. The test
 * issues the same listDatabases and listCollections commands that collection cloning does so we
 * know all the commands that will be run on the sync source and can verify that only one is run per
 * call to step(). Similarly for oplog application, we can check the log messages to make sure that
 * the batches being applied are of the expected size and that only one batch was applied per step()
 * call.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";

    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/libs/check_log.js");
    load("jstests/replsets/libs/initial_sync_test.js");

    /**
     * Helper function to check that specific messages appeared or did not appear in the logs. If
     * the command was listIndexes and we expect the message to appear, we also add the collection
     * UUID to make sure that it corresponds to the expected collection.
     */
    function checkLogForCollectionClonerMsg(node, commandName, dbname, contains, collUUID) {
        let msg = "Collection Cloner scheduled a remote command on the " + dbname + " db: { " +
            commandName;
        if (commandName === "listIndexes" && contains) {
            msg += ": " + collUUID;
        }

        if (contains) {
            assert(checkLog.checkContainsOnce(node, msg));
        } else {
            assert(!checkLog.checkContainsOnce(node, msg));
        }
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
    const initialSyncTest = new InitialSyncTest();
    const primary = initialSyncTest.getPrimary();
    let secondary = initialSyncTest.getSecondary();
    const db = primary.getDB("test");
    const largeString = 'z'.repeat(10 * 1024 * 1024);

    assert.commandWorked(db.foo.insert({a: 1}));
    assert.commandWorked(db.bar.insert({b: 1}));

    // Prepare a transaction so that we know that step() can restart the secondary even if there is
    // a prepared transaction.
    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.getCollection("foo");
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({c: 1}));
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // Do same listDatabases command as CollectionCloner.
    const databases =
        assert.commandWorked(primary.adminCommand({listDatabases: 1, nameOnly: true})).databases;

    // This step call restarts the secondary and causes it to go into initial sync.
    assert(!initialSyncTest.step());

    secondary = initialSyncTest.getSecondary();
    secondary.setSlaveOk();

    // Make sure that we cannot read from this node yet.
    assert.commandFailedWithCode(secondary.getDB("test").runCommand({count: "foo"}),
                                 ErrorCodes.NotMasterOrSecondary);

    // Make sure that we saw the listDatabases call in the log messages, but didn't see any
    // listCollections or listIndexes call.
    checkLogForCollectionClonerMsg(secondary, "listDatabases", "admin", true);
    checkLogForCollectionClonerMsg(secondary, "listCollections", "admin", false);
    checkLogForCollectionClonerMsg(secondary, "listIndexes", "admin", false);

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
                                     ErrorCodes.NotMasterOrSecondary);

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
                                         ErrorCodes.NotMasterOrSecondary);

            // Make sure that we saw the listIndexes call in the log messages, but didn't
            // see a listCollections call.
            checkLogForCollectionClonerMsg(secondary, "listIndexes", dbname, true, collUUID);
            checkLogForCollectionClonerMsg(secondary, "listCollections", "admin", false);

            // Perform large operations during collection cloning so that we will need multiple
            // batches during oplog application.
            assert.commandWorked(db.foo.insert({d: largeString}));
            assert.commandWorked(db.bar.insert({e: largeString}));
        }
    }

    // Check that we see the expected number of batches during oplog application.

    // This batch should correspond to the 'prepare' op.
    assert(!initialSyncTest.step());
    checkLogForOplogApplicationMsg(secondary, 1);
    assert(!initialSyncTest.step());
    checkLogForOplogApplicationMsg(secondary, 9);
    assert(!initialSyncTest.step());
    checkLogForOplogApplicationMsg(secondary, 1);

    assert(initialSyncTest.step(), "Expected initial sync to have completed, but it did not");

    // Abort transaction so that the data consistency checks in stop() can run.
    assert.commandWorked(session.abortTransaction_forTesting());

    // Issue a w:2 write to make sure the secondary has replicated the abortTransaction oplog entry.
    assert.commandWorked(primary.getDB("otherDB").otherColl.insert({x: 1}, {writeConcern: {w: 2}}));

    // Confirm that node can be read from and that it has the inserts that were made while the node
    // was in initial sync.
    assert.eq(secondary.getDB("test").foo.find().count(), 6);
    assert.eq(secondary.getDB("test").bar.find().count(), 6);
    assert.eq(secondary.getDB("test").foo.find().itcount(), 6);
    assert.eq(secondary.getDB("test").bar.find().itcount(), 6);

    // Do data consistency checks at the end.
    initialSyncTest.stop();

})();
