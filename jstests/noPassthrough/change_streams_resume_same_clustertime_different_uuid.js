/**
 * Confirms that resuming from an event which has the same clusterTime but a different UUID than on
 * another shard does not cause the resume attempt to be prematurely rejected. Reproduction script
 * for the bug described in SERVER-40094.
 * @tags: [requires_sharding, uses_change_streams]
 */
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");  // For runCommandOnEachPrimary.

    // Asserts that the expected operation type and documentKey are found on the change stream
    // cursor. Returns the change stream document.
    function assertWriteVisible({cursor, opType, docKey}) {
        assert.soon(() => cursor.hasNext());
        const changeDoc = cursor.next();
        assert.eq(opType, changeDoc.operationType, changeDoc);
        assert.eq(docKey, changeDoc.documentKey, changeDoc);
        return changeDoc;
    }

    // Create a new cluster with 2 shards. Disable periodic no-ops to ensure that we have control
    // over the ordering of events across the cluster.
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: false, periodicNoopIntervalSecs: 1}}
    });

    // Create two databases. We will place one of these on each shard.
    const mongosDB0 = st.s.getDB(`${jsTestName()}_0`);
    const mongosDB1 = st.s.getDB(`${jsTestName()}_1`);
    const adminDB = st.s.getDB("admin");

    // Enable sharding on mongosDB0 and ensure its primary is shard0.
    assert.commandWorked(mongosDB0.adminCommand({enableSharding: mongosDB0.getName()}));
    st.ensurePrimaryShard(mongosDB0.getName(), st.rs0.getURL());

    // Enable sharding on mongosDB1 and ensure its primary is shard1.
    assert.commandWorked(mongosDB1.adminCommand({enableSharding: mongosDB1.getName()}));
    st.ensurePrimaryShard(mongosDB1.getName(), st.rs1.getURL());

    // Open a connection to a different collection on each shard. We use direct connections to
    // ensure that the oplog timestamps across the shards overlap.
    const coll0 = st.rs0.getPrimary().getCollection(`${mongosDB0.getName()}.test`);
    const coll1 = st.rs1.getPrimary().getCollection(`${mongosDB1.getName()}.test`);

    // Open a change stream on the test cluster. We will capture events in 'changeList'.
    const changeStreamCursor = adminDB.aggregate([{$changeStream: {allChangesForCluster: true}}]);
    const changeList = [];

    // Insert ten documents on each shard, alternating between the two collections.
    for (let i = 0; i < 20; ++i) {
        const coll = (i % 2 ? coll1 : coll0);
        assert.commandWorked(coll.insert({shard: (i % 2)}));
    }

    // Verify that each shard now has ten total documents present in the associated collection.
    assert.eq(st.rs0.getPrimary().getCollection(coll0.getFullName()).count(), 10);
    assert.eq(st.rs1.getPrimary().getCollection(coll1.getFullName()).count(), 10);

    // Re-enable 'writePeriodicNoops' to ensure that all change stream events are returned.
    FixtureHelpers.runCommandOnEachPrimary(
        {db: adminDB, cmdObj: {setParameter: 1, writePeriodicNoops: true}});

    // Read the stream of events, capture them in 'changeList', and confirm that all events occurred
    // at or later than the clusterTime of the first event. Unfortunately, we cannot guarantee that
    // corresponding events occurred at the same clusterTime on both shards; we expect, however,
    // that this will be true in the vast majority of runs, and so there is value in testing.
    for (let i = 0; i < 20; ++i) {
        assert.soon(() => changeStreamCursor.hasNext());
        changeList.push(changeStreamCursor.next());
    }
    const clusterTime = changeList[0].clusterTime;
    for (let event of changeList) {
        assert.gte(event.clusterTime, clusterTime);
    }

    // Test that resuming from each event returns the expected set of subsequent documents.
    for (let i = 0; i < changeList.length; ++i) {
        const resumeCursor = adminDB.aggregate(
            [{$changeStream: {allChangesForCluster: true, resumeAfter: changeList[i]._id}}]);

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
        resumeCursor.close();
    }

    st.stop();
})();
