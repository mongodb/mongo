// Confirms that change streams only see committed operations for sharded transactions.
// @tags: [
//   requires_sharding,
//   uses_change_streams,
//   uses_multi_shard_transaction,
//   uses_transactions,
//   exclude_from_large_txns_due_to_change_streams,
// ]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "change_stream_transaction_sharded";
    const namespace = dbName + "." + collName;

    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
    });

    const mongosConn = st.s;
    assert.commandWorked(mongosConn.getDB(dbName).getCollection(collName).createIndex({shard: 1}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    // Shard the test collection and split it into two chunks: one that contains all {shard: 1}
    // documents and one that contains all {shard: 2} documents.
    st.shardColl(collName,
                 {shard: 1} /* shard key */,
                 {shard: 2} /* split at */,
                 {shard: 2} /* move the chunk containing {shard: 2} to its own shard */,
                 dbName,
                 true);
    // Seed each chunk with an initial document.
    assert.commandWorked(mongosConn.getDB(dbName).getCollection(collName).insert(
        {shard: 1}, {writeConcern: {w: "majority"}}));
    assert.commandWorked(mongosConn.getDB(dbName).getCollection(collName).insert(
        {shard: 2}, {writeConcern: {w: "majority"}}));

    const db = mongosConn.getDB(dbName);
    const coll = db.getCollection(collName);
    let changeListShard1 = [], changeListShard2 = [];

    //
    // Start transaction 1.
    //
    const session1 = db.getMongo().startSession({causalConsistency: true});
    const sessionDb1 = session1.getDatabase(dbName);
    const sessionColl1 = sessionDb1[collName];
    session1.startTransaction({readConcern: {level: "majority"}});

    //
    // Start transaction 2.
    //
    const session2 = db.getMongo().startSession({causalConsistency: true});
    const sessionDb2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDb2[collName];
    session2.startTransaction({readConcern: {level: "majority"}});

    /**
     * Asserts that there are no changes waiting on the change stream cursor.
     */
    function assertNoChanges(cursor) {
        assert(!cursor.hasNext(), () => {
            return "Unexpected change set: " + tojson(cursor.toArray());
        });
    }

    //
    // Perform writes both in and outside of transactions and confirm that the changes expected are
    // returned by the change stream.
    //
    (function() {
        /**
         * Asserts that the expected changes are found on the change stream cursor. Pushes the
         * corresponding change stream document (with resume token) to an array. When expected
         * changes are provided for both shards, we must assume that either shard's changes could
         * come first or that they are interleaved via applyOps index. This is because a cross shard
         * transaction may commit at a different cluster time on each shard, which impacts the
         * ordering of the change stream.
         */
        function assertWritesVisibleWithCapture(cursor,
                                                expectedChangesShard1,
                                                expectedChangesShard2,
                                                changeCaptureListShard1,
                                                changeCaptureListShard2) {
            function assertChangeEqualWithCapture(changeDoc, expectedChange, changeCaptureList) {
                assert.eq(expectedChange.operationType, changeDoc.operationType);
                assert.eq(expectedChange._id, changeDoc.documentKey._id);
                changeCaptureList.push(changeDoc);
            }

            while (expectedChangesShard1.length || expectedChangesShard2.length) {
                assert.soon(() => cursor.hasNext());
                const changeDoc = cursor.next();

                if (changeDoc.documentKey.shard === 1) {
                    assert(expectedChangesShard1.length);
                    assertChangeEqualWithCapture(
                        changeDoc, expectedChangesShard1[0], changeCaptureListShard1);
                    expectedChangesShard1.shift();
                } else {
                    assert(changeDoc.documentKey.shard === 2);
                    assert(expectedChangesShard2.length);
                    assertChangeEqualWithCapture(
                        changeDoc, expectedChangesShard2[0], changeCaptureListShard2);
                    expectedChangesShard2.shift();
                }
            }

            assertNoChanges(cursor);
        }

        // Open a change stream on the test collection.
        const changeStreamCursor = coll.watch();

        // Insert a document and confirm that the change stream has it.
        assert.commandWorked(
            coll.insert({shard: 1, _id: "no-txn-doc-1"}, {writeConcern: {w: "majority"}}));
        assertWritesVisibleWithCapture(changeStreamCursor,
                                       [{operationType: "insert", _id: "no-txn-doc-1"}],
                                       [],
                                       changeListShard1,
                                       changeListShard2);

        // Insert two documents under each transaction and confirm no change stream updates.
        assert.commandWorked(
            sessionColl1.insert([{shard: 1, _id: "txn1-doc-1"}, {shard: 2, _id: "txn1-doc-2"}]));
        assert.commandWorked(
            sessionColl2.insert([{shard: 1, _id: "txn2-doc-1"}, {shard: 2, _id: "txn2-doc-2"}]));
        assertNoChanges(changeStreamCursor);

        // Update one document under each transaction and confirm no change stream updates.
        assert.commandWorked(
            sessionColl1.update({shard: 1, _id: "txn1-doc-1"}, {$set: {"updated": 1}}));
        assert.commandWorked(
            sessionColl2.update({shard: 2, _id: "txn2-doc-2"}, {$set: {"updated": 1}}));
        assertNoChanges(changeStreamCursor);

        // Update and then remove second doc under each transaction.
        assert.commandWorked(sessionColl1.update({shard: 2, _id: "txn1-doc-2"},
                                                 {$set: {"update-before-delete": 1}}));
        assert.commandWorked(sessionColl2.update({shard: 1, _id: "txn2-doc-1"},
                                                 {$set: {"update-before-delete": 1}}));
        assert.commandWorked(sessionColl1.remove({shard: 2, _id: "txn1-doc-2"}));
        assert.commandWorked(sessionColl2.remove({shard: 1, _id: "txn2-doc-2"}));
        assertNoChanges(changeStreamCursor);

        // Perform a write outside of a transaction and confirm that the change stream sees only
        // this write.
        assert.commandWorked(
            coll.insert({shard: 2, _id: "no-txn-doc-2"}, {writeConcern: {w: "majority"}}));
        assertWritesVisibleWithCapture(changeStreamCursor,
                                       [],
                                       [{operationType: "insert", _id: "no-txn-doc-2"}],
                                       changeListShard1,
                                       changeListShard2);
        assertNoChanges(changeStreamCursor);

        // Perform a write outside of the transaction.
        assert.commandWorked(
            coll.insert({shard: 1, _id: "no-txn-doc-3"}, {writeConcern: {w: "majority"}}));

        // Commit first transaction and confirm that the change stream sees the changes expected
        // from each shard.
        session1.commitTransaction();
        assertWritesVisibleWithCapture(changeStreamCursor,
                                       [
                                         {operationType: "insert", _id: "no-txn-doc-3"},
                                         {operationType: "insert", _id: "txn1-doc-1"},
                                         {operationType: "update", _id: "txn1-doc-1"}
                                       ],
                                       [
                                         {operationType: "insert", _id: "txn1-doc-2"},
                                         {operationType: "update", _id: "txn1-doc-2"},
                                         {operationType: "delete", _id: "txn1-doc-2"}
                                       ],
                                       changeListShard1,
                                       changeListShard2);
        assertNoChanges(changeStreamCursor);

        // Perform a write outside of the transaction.
        assert.commandWorked(
            coll.insert({shard: 2, _id: "no-txn-doc-4"}, {writeConcern: {w: "majority"}}));

        // Abort second transaction and confirm that the change stream sees only the previous
        // non-transaction write.
        session2.abortTransaction();
        assertWritesVisibleWithCapture(changeStreamCursor,
                                       [],
                                       [{operationType: "insert", _id: "no-txn-doc-4"}],
                                       changeListShard1,
                                       changeListShard2);
        assertNoChanges(changeStreamCursor);
        changeStreamCursor.close();
    })();

    //
    // Open a change stream at each resume point captured for the previous writes. Confirm that the
    // documents returned match what was returned for the initial change stream.
    //
    (function() {

        /**
         * Iterates over a list of changes and returns the index of the change whose resume token is
         * higher than that of 'changeDoc'. It is expected that 'changeList' entries at this index
         * and beyond will be included in a change stream resumed at 'changeDoc._id'.
         */
        function getPostTokenChangeIndex(changeDoc, changeList) {
            for (let i = 0; i < changeList.length; ++i) {
                if (changeDoc._id._data < changeList[i]._id._data) {
                    return i;
                }
            }

            return changeList.length;
        }

        /**
         * Confirms that the change represented by 'changeDoc' exists in 'shardChangeList' at index
         * 'changeListIndex'.
         */
        function shardHasDocumentAtChangeListIndex(changeDoc, shardChangeList, changeListIndex) {
            assert(changeListIndex < shardChangeList.length);

            const expectedChangeDoc = shardChangeList[changeListIndex];
            assert.eq(changeDoc, expectedChangeDoc);
            assert.eq(expectedChangeDoc.documentKey,
                      changeDoc.documentKey,
                      tojson(changeDoc) + ", " + tojson(expectedChangeDoc));
        }

        /**
         * Test that change stream returns the expected set of documuments when resumed from each
         * point captured by 'changeList'.
         */
        function confirmResumeForChangeList(changeList, changeListShard1, changeListShard2) {
            for (let i = 0; i < changeList.length; ++i) {
                const resumeDoc = changeList[i];
                let indexShard1 = getPostTokenChangeIndex(resumeDoc, changeListShard1);
                let indexShard2 = getPostTokenChangeIndex(resumeDoc, changeListShard2);
                const resumeCursor = coll.watch([], {startAfter: resumeDoc._id});

                while ((indexShard1 + indexShard2) <
                       (changeListShard1.length + changeListShard2.length)) {
                    assert.soon(() => resumeCursor.hasNext());
                    const changeDoc = resumeCursor.next();

                    if (changeDoc.documentKey.shard === 1) {
                        shardHasDocumentAtChangeListIndex(
                            changeDoc, changeListShard1, indexShard1++);
                    } else {
                        assert(changeDoc.documentKey.shard === 2);
                        shardHasDocumentAtChangeListIndex(
                            changeDoc, changeListShard2, indexShard2++);
                    }
                }

                assertNoChanges(resumeCursor);
                resumeCursor.close();
            }
        }

        // Confirm that the sequence of events returned by the stream is consistent when resuming
        // from any point in the stream on either shard.
        confirmResumeForChangeList(changeListShard1, changeListShard1, changeListShard2);
        confirmResumeForChangeList(changeListShard2, changeListShard1, changeListShard2);
    })();

    st.stop();
})();
