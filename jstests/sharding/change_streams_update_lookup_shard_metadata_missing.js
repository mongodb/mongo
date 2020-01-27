/**
 * Tests that an updateLookup change stream doesn't throw ChangeStreamFatalError after
 * fixing SERVER-44598
 *
 * @tags: [uses_change_streams]
 */
(function() {
    "use strict";

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
    assert.commandWorked(mongosColl.insert({_id: 0, a: -100}));
    assert.soon(() => csCursor.hasNext());

    const resumeToken = csCursor.next()._id;

    // get any secondary
    const newPrimary = st.rs0.getSecondary();
    let shards = st.s.getDB('config').shards.find().toArray();

    // Step up one of the Secondaries, which will not have any sharding metadata loaded.
    st.rs0.stepUpNoAwaitReplication(newPrimary);

    // make sure the mongos refreshes it's connections to the shard
    let primary = {};
    do {
        let connPoolStats = st.s0.adminCommand({connPoolStats: 1});
        primary = connPoolStats.replicaSets[shards[0]._id].hosts.find((host) => {
            return host.ismaster;
        }) ||
            {};
    } while (newPrimary.host !== primary.addr);

    // Do a {multi:true} update. This will scatter to all shards and update the document on shard0.
    // Because no metadata is loaded, the shard will return a StaleShardVersion and fetch it,
    // the operation will be retried
    assert.soonNoExcept(() => assert.commandWorked(
                            mongosColl.update({_id: 0}, {$set: {updated: true}}, false, true)));

    // Resume the change stream with {fullDocument: 'updateLookup'}.
    const cmdRes = assert.commandWorked(mongosColl.runCommand("aggregate", {
        pipeline: [{$changeStream: {resumeAfter: resumeToken, fullDocument: "updateLookup"}}],
        cursor: {}
    }));

    assert.soon(() => csCursor.hasNext());

    const updateObj = csCursor.next();

    assert.eq(true, updateObj.updateDescription.updatedFields.updated);

    st.stop();
})();