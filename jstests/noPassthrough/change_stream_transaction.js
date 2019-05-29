// Confirms that change streams only see committed operations for prepared transactions.
// @tags: [uses_transactions,uses_change_streams,requires_majority_read_concern,
// exclude_from_large_txns_due_to_change_streams]
(function() {
    "use strict";

    load("jstests/core/txns/libs/prepare_helpers.js");  // For PrepareHelpers.

    const dbName = "test";
    const collName = "change_stream_transaction";

    /**
     * Asserts that the expected operation type and documentKey are found on the change stream
     * cursor. Returns the change stream document.
     */
    function assertWriteVisible(cursor, operationType, documentKey) {
        assert.soon(() => cursor.hasNext());
        const changeDoc = cursor.next();
        assert.eq(operationType, changeDoc.operationType, changeDoc);
        assert.eq(documentKey, changeDoc.documentKey, changeDoc);
        return changeDoc;
    }

    /**
     * Asserts that the expected operation type and documentKey are found on the change stream
     * cursor. Pushes the corresponding resume token and change stream document to an array.
     */
    function assertWriteVisibleWithCapture(cursor, operationType, documentKey, changeList) {
        const changeDoc = assertWriteVisible(cursor, operationType, documentKey);
        changeList.push(changeDoc);
    }

    /**
     * Asserts that there are no changes waiting on the change stream cursor.
     */
    function assertNoChanges(cursor) {
        assert(!cursor.hasNext(), () => {
            return "Unexpected change set: " + tojson(cursor.toArray());
        });
    }

    function runTest(conn) {
        const db = conn.getDB(dbName);
        const coll = db.getCollection(collName);
        const unwatchedColl = db.getCollection(collName + "_unwatched");
        let changeList = [];

        // Collections must be created outside of any transaction.
        assert.commandWorked(db.createCollection(coll.getName()));
        assert.commandWorked(db.createCollection(unwatchedColl.getName()));

        //
        // Start transaction 1.
        //
        const session1 = db.getMongo().startSession();
        const sessionDb1 = session1.getDatabase(dbName);
        const sessionColl1 = sessionDb1[collName];
        session1.startTransaction({readConcern: {level: "majority"}});

        //
        // Start transaction 2.
        //
        const session2 = db.getMongo().startSession();
        const sessionDb2 = session2.getDatabase(dbName);
        const sessionColl2 = sessionDb2[collName];
        session2.startTransaction({readConcern: {level: "majority"}});

        //
        // Start transaction 3.
        //
        const session3 = db.getMongo().startSession();
        const sessionDb3 = session3.getDatabase(dbName);
        const sessionColl3 = sessionDb3[collName];
        session3.startTransaction({readConcern: {level: "majority"}});

        // Open a change stream on the test collection.
        const changeStreamCursor = coll.watch();

        // Insert a document and confirm that the change stream has it.
        assert.commandWorked(coll.insert({_id: "no-txn-doc-1"}, {writeConcern: {w: "majority"}}));
        assertWriteVisibleWithCapture(
            changeStreamCursor, "insert", {_id: "no-txn-doc-1"}, changeList);

        // Insert two documents under each transaction and confirm no change stream updates.
        assert.commandWorked(sessionColl1.insert([{_id: "txn1-doc-1"}, {_id: "txn1-doc-2"}]));
        assert.commandWorked(sessionColl2.insert([{_id: "txn2-doc-1"}, {_id: "txn2-doc-2"}]));
        assertNoChanges(changeStreamCursor);

        // Update one document under each transaction and confirm no change stream updates.
        assert.commandWorked(sessionColl1.update({_id: "txn1-doc-1"}, {$set: {"updated": 1}}));
        assert.commandWorked(sessionColl2.update({_id: "txn2-doc-1"}, {$set: {"updated": 1}}));
        assertNoChanges(changeStreamCursor);

        // Update and then remove the second doc under each transaction and confirm no change stream
        // events are seen.
        assert.commandWorked(
            sessionColl1.update({_id: "txn1-doc-2"}, {$set: {"update-before-delete": 1}}));
        assert.commandWorked(
            sessionColl2.update({_id: "txn2-doc-2"}, {$set: {"update-before-delete": 1}}));
        assert.commandWorked(sessionColl1.remove({_id: "txn1-doc-2"}));
        assert.commandWorked(sessionColl2.remove({_id: "txn2-doc-2"}));
        assertNoChanges(changeStreamCursor);

        // Perform a write to the 'session1' transaction in a collection that is not being watched
        // by 'changeStreamCursor'. We do not expect to see this write in the change stream either
        // now or on commit.
        assert.commandWorked(
            sessionDb1[unwatchedColl.getName()].insert({_id: "txn1-doc-unwatched-collection"}));
        assertNoChanges(changeStreamCursor);

        // Perform a write to the 'session3' transaction in a collection that is not being watched
        // by 'changeStreamCursor'. We do not expect to see this write in the change stream either
        // now or on commit.
        assert.commandWorked(
            sessionDb3[unwatchedColl.getName()].insert({_id: "txn3-doc-unwatched-collection"}));
        assertNoChanges(changeStreamCursor);

        // Perform a write outside of a transaction and confirm that the change stream sees only
        // this write.
        assert.commandWorked(coll.insert({_id: "no-txn-doc-2"}, {writeConcern: {w: "majority"}}));
        assertWriteVisibleWithCapture(
            changeStreamCursor, "insert", {_id: "no-txn-doc-2"}, changeList);
        assertNoChanges(changeStreamCursor);

        let prepareTimestampTxn1;
        prepareTimestampTxn1 = PrepareHelpers.prepareTransaction(session1);
        assertNoChanges(changeStreamCursor);

        // TODO SERVER-39036: Change writeConcern to majority. Prior to this ticket a majority write
        // will block on a prepared transaction. We should also be able to move the check for
        // document existence prior to the transaction commit with this change.
        // Perform a write at writeConcern w: local.
        assert.commandWorked(coll.insert({_id: "no-txn-doc-3"}, {writeConcern: {w: 1}}));

        //
        // Commit first transaction and confirm expected changes.
        //
        assert.commandWorked(PrepareHelpers.commitTransaction(session1, prepareTimestampTxn1));
        assertWriteVisibleWithCapture(
            changeStreamCursor, "insert", {_id: "no-txn-doc-3"}, changeList);
        assertWriteVisibleWithCapture(
            changeStreamCursor, "insert", {_id: "txn1-doc-1"}, changeList);
        assertWriteVisibleWithCapture(
            changeStreamCursor, "insert", {_id: "txn1-doc-2"}, changeList);
        assertWriteVisibleWithCapture(
            changeStreamCursor, "update", {_id: "txn1-doc-1"}, changeList);
        assertWriteVisibleWithCapture(
            changeStreamCursor, "update", {_id: "txn1-doc-2"}, changeList);
        assertWriteVisibleWithCapture(
            changeStreamCursor, "delete", {_id: "txn1-doc-2"}, changeList);
        assertNoChanges(changeStreamCursor);

        // Transition the second transaction to prepared. We skip capturing the prepare
        // timestamp it is not required for abortTransaction_forTesting().
        PrepareHelpers.prepareTransaction(session2);
        assertNoChanges(changeStreamCursor);

        // TODO SERVER-39036: Change writeConcern to majority. Prior to this ticket a majority write
        // will block on a prepared transaction. We should also be able to move the check for
        // document existence prior to the transaction abort with this change.
        // Perform a write at writeConcern w: local.
        assert.commandWorked(coll.insert({_id: "no-txn-doc-4"}, {writeConcern: {w: 1}}));

        //
        // Abort second transaction.
        //
        assert.commandWorked(session2.abortTransaction_forTesting());
        assertWriteVisibleWithCapture(
            changeStreamCursor, "insert", {_id: "no-txn-doc-4"}, changeList);
        assertNoChanges(changeStreamCursor);
        changeStreamCursor.close();

        // Test that change stream resume returns the expected set of documents at each point
        // captured by this test.
        for (let i = 0; i < changeList.length; ++i) {
            const resumeCursor = coll.watch([], {startAfter: changeList[i]._id});

            for (let x = (i + 1); x < changeList.length; ++x) {
                const expectedChangeDoc = changeList[x];
                assertWriteVisible(
                    resumeCursor, expectedChangeDoc.operationType, expectedChangeDoc.documentKey);
            }

            assertNoChanges(resumeCursor);
            resumeCursor.close();
        }

        //
        // Prepare and commit the third transaction and confirm that there are no visible changes.
        //
        let prepareTimestampTxn3;
        prepareTimestampTxn3 = PrepareHelpers.prepareTransaction(session3);
        assertNoChanges(changeStreamCursor);

        assert.commandWorked(PrepareHelpers.commitTransaction(session3, prepareTimestampTxn3));
        assertNoChanges(changeStreamCursor);

        assert.commandWorked(db.dropDatabase());
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    runTest(rst.getPrimary());

    rst.stopSet();
})();
