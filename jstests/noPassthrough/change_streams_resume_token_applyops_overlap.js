/**
 * Confirms that resuming from an event which has the same clusterTime as a transaction on another
 * shard does not cause the resume attempt to be prematurely rejected. Reproduction script for the
 * bug described in SERVER-40094.
 * @tags: [requires_sharding, uses_multi_shard_transaction, uses_transactions,
 * exclude_from_large_txns]
 */
(function() {
    "use strict";

    // Asserts that the expected operation type and documentKey are found on the change stream
    // cursor. Returns the change stream document.
    function assertWriteVisible({cursor, opType, docKey}) {
        assert.soon(() => cursor.hasNext());
        const changeDoc = cursor.next();
        assert.eq(opType, changeDoc.operationType, changeDoc);
        assert.eq(docKey, changeDoc.documentKey, changeDoc);
        return changeDoc;
    }

    // Create a new cluster with 2 shards. Enable 1-second period no-ops to ensure that all relevant
    // events eventually become available.
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
    });

    const mongosDB = st.s.getDB(jsTestName());
    const mongosColl = mongosDB.test;

    // Enable sharding on the test DB and ensure its primary is shard0.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard on {shard:1}, split at {shard:1}, and move the upper chunk to shard1.
    st.shardColl(mongosColl, {shard: 1}, {shard: 1}, {shard: 1}, mongosDB.getName(), true);

    // Seed each shard with one document.
    assert.commandWorked(
        mongosColl.insert([{shard: 0, _id: "initial_doc"}, {shard: 1, _id: "initial doc"}]));

    // Start a transaction which will be used to write documents across both shards.
    const session = mongosDB.getMongo().startSession();
    const sessionDB = session.getDatabase(mongosDB.getName());
    const sessionColl = sessionDB[mongosColl.getName()];
    session.startTransaction({readConcern: {level: "majority"}});

    // Open a change stream on the test collection. We will capture events in 'changeList'.
    const changeStreamCursor = mongosColl.watch();
    const changeList = [];

    // Insert four documents on each shard under the transaction.
    assert.commandWorked(
        sessionColl.insert([{shard: 0, _id: "txn1-doc-0"}, {shard: 1, _id: "txn1-doc-1"}]));
    assert.commandWorked(
        sessionColl.insert([{shard: 0, _id: "txn1-doc-2"}, {shard: 1, _id: "txn1-doc-3"}]));
    assert.commandWorked(
        sessionColl.insert([{shard: 0, _id: "txn1-doc-4"}, {shard: 1, _id: "txn1-doc-5"}]));
    assert.commandWorked(
        sessionColl.insert([{shard: 0, _id: "txn1-doc-6"}, {shard: 1, _id: "txn1-doc-7"}]));

    // Commit the transaction.
    session.commitTransaction();

    // Read the stream of events, capture them in 'changeList', and confirm that all events occurred
    // at or later than the clusterTime of the first event. Unfortunately, we cannot guarantee that
    // all events occurred at the same clusterTime on both shards, even in the case where all events
    // occur within a single transaction. We expect, however, that this will be true in the vast
    // majority of test runs, and so there is value in retaining this test.
    for (let i = 0; i < 8; ++i) {
        assert.soon(() => changeStreamCursor.hasNext());
        changeList.push(changeStreamCursor.next());
    }
    const clusterTime = changeList[0].clusterTime;
    for (let event of changeList) {
        assert.gte(event.clusterTime, clusterTime);
    }

    // Test that resuming from each event returns the expected set of subsequent documents.
    for (let i = 0; i < changeList.length; ++i) {
        const resumeCursor = mongosColl.watch([], {startAfter: changeList[i]._id});

        // Confirm that the first event in the resumed stream matches the next event recorded in
        // 'changeList' from the original stream. The order of the events should be stable across
        // resumes from any point.
        for (let x = (i + 1); x < changeList.length; ++x) {
            const expectedChangeDoc = changeList[x];
            assertWriteVisible({
                cursor: resumeCursor,
                opType: expectedChangeDoc.operationType,
                docKey: expectedChangeDoc.documentKey
            });
        }
        assert(!resumeCursor.hasNext(), () => `Unexpected event: ${tojson(resumeCursor.next())}`);
        resumeCursor.close();
    }

    st.stop();
})();
