/**
 * Tests that single-collection high water mark tokens on a sharded cluster always contain the
 * collection's UUID, even if the collection is not present on all shards.
 * @tags: [requires_replication, requires_journaling]
 */
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    // Create a 2-shard cluster. Disable 'writePeriodicNoops' on the shards, since we want to
    // manually control which shard advances and when.
    const st =
        new ShardingTest({shards: 2, rs: {nodes: 1, setParameter: {writePeriodicNoops: false}}});

    // Obtain a connection to the mongoS and one direct connection to each shard.
    const shard0 = st.rs0.getPrimary();
    const shard1 = st.rs1.getPrimary();
    const mongos = st.s;

    const mongosDB = mongos.getDB(jsTestName());
    const mongosColl = mongosDB.test;

    const shard1DB = shard1.getDB(jsTestName());
    const shard1Coll = shard1DB.test;

    const shard1UnrelatedColl = shard1.getCollection("otherdb.unrelated_collection");

    const shardNames = [st.rs0.name, st.rs1.name];

    function runHighWaterMarkTest(testCollOnlyOnShard0) {
        // Open a stream on the test collection, and get the first available high-water-mark.
        const csCursor = testCollOnlyOnShard0.watch();
        const firstHWM = csCursor.getResumeToken();
        assert(!csCursor.hasNext());
        // Write a document to the unrelated collection on shard1 to push its clusterTime forward.
        assert.commandWorked(shard1UnrelatedColl.insert({}));
        assert(!csCursor.hasNext());
        // Write a document to the test collection on shard0, advancing its optime to the present.
        assert.commandWorked(testCollOnlyOnShard0.insert({}));
        assert(!csCursor.hasNext());
        // Wait for the high water mark token to advance, and confirm that we do not see any events.
        // This token is guaranteed to be from shard1, which does not have the collection, because
        // otherwise we would have been able to retrieve the document we just wrote.
        assert.soon(() => {
            assert(!shard1Coll.exists());
            assert(!csCursor.hasNext());
            return bsonWoCompare(csCursor.getResumeToken(), firstHWM) > 0;
        });
        // Confirm that we can resumeAfter a token from the shard that does not have the collection.
        const hwmFromShardWithoutCollection = csCursor.getResumeToken();
        assert.commandWorked(testCollOnlyOnShard0.runCommand({
            aggregate: testCollOnlyOnShard0.getName(),
            pipeline: [{$changeStream: {resumeAfter: hwmFromShardWithoutCollection}}],
            cursor: {}
        }));
    }

    // Enable sharding on the the test database and ensure that the primary is shard0.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), shard0.name);

    // Create an unsharded collection on shard0 and confirm that HWM tokens return from a stream on
    // this shard have the correct UUIDs, despite the fact that the stream is opened on all shards.
    assertCreateCollection(mongosDB, mongosColl.getName());
    runHighWaterMarkTest(mongosColl);

    // Shard the collection on {_id: 1}, keep it on shard0 only, and re-run the test.
    st.shardColl(mongosColl, {_id: 1}, false, false);
    runHighWaterMarkTest(mongosColl);

    st.stop();
})();