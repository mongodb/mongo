/**
 * Tests that an updateLookup change stream throws ChangeStreamFatalError when it encounters an
 * oplog entry whose documentKey omits the shard key.
 * TODO SERVER-44598: the oplog entry will no longer omit the shard key when SERVER-44598 is fixed,
 * and so this test will no longer be relevant.
 * @tags: [uses_change_streams, requires_majority_read_concern]
 */
(function() {
    "use strict";

    load(
        "jstests/multiVersion/libs/causal_consistency_helpers.js");  // supportsMajorityReadConcern.

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // The UUID consistency check can hit NotMasterNoSlaveOk when it attempts to obtain a list of
    // collections from the shard Primaries through mongoS at the end of this test.
    TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

    // Start a new sharded cluster with 2 nodes and obtain references to the test DB and collection.
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 3, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
    });

    const mongosDB = st.s.getDB(jsTestName());
    const mongosColl = mongosDB.test;
    const shard0 = st.rs0;

    // Enable sharding on the the test database and ensure that the primary is shard0.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), shard0.getURL());

    // Shard the source collection on {a: 1}, split across the shards at {a: 0}.
    st.shardColl(mongosColl, {a: 1}, {a: 0}, {a: 1});

    // Open a change stream on the collection.
    const csCursor = mongosColl.watch();

    // Write one document onto shard0 and obtain its resume token.
    assert.writeOK(mongosColl.insert({_id: 0, a: -100}));
    assert.soon(() => csCursor.hasNext());

    const resumeToken = csCursor.next()._id;

    // Step up one of the Secondaries, which will not have any sharding metadata loaded.
    assert.commandWorked(shard0.getSecondary().adminCommand({replSetStepUp: 1}));
    shard0.awaitNodesAgreeOnPrimary();

    // Do a {multi:true} update. This will scatter to all shards and update the document on shard0.
    // Because no metadata is loaded, this will write the update into the oplog with a documentKey
    // containing only the _id field.
    assert.soonNoExcept(
        () => assert.writeOK(mongosColl.update({_id: 0}, {$set: {updated: true}}, false, true)));

    // Resume the change stream with {fullDocument: 'updateLookup'}.
    const cmdRes = assert.commandWorked(mongosColl.runCommand("aggregate", {
        pipeline: [{$changeStream: {resumeAfter: resumeToken, fullDocument: "updateLookup"}}],
        cursor: {}
    }));

    // Begin pulling from the stream. We should hit a ChangeStreamFatalError when the updateLookup
    // attempts to read the update entry that is missing the shard key value of the document.
    assert.soonNoExcept(
        () => assert.commandFailedWithCode(
            mongosColl.runCommand({getMore: cmdRes.cursor.id, collection: mongosColl.getName()}),
            ErrorCodes.ChangeStreamFatalError));

    st.stop();
})();