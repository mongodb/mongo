/**
 * Tests that after a restart on a shard a multi-write operation will not be repeated
 *
 * This test requrires persistence because it asumes the shard will still have it's
 * data after restarting
 *
 * @tags: [requires_persistence]
 */
(function() {
    'use strict';
    var st = new ShardingTest({shards: 2, mongos: 1});

    st.enableSharding('TestDB');
    st.ensurePrimaryShard('TestDB', st.rs1.getURL());

    // Hash shard the collection so that the chunks are placed uniformly across the shards from the
    // beginning (because the collection is empty)
    st.shardCollection('TestDB.TestColl', {Key: 'hashed'}, false, {numInitialChunks: 120});
    // Helper function to restart a node and wait that the entire set is operational

    // Insert a document outside the shard key range so the update below will generate a scather-
    // gather write operation
    st.s0.getDB('TestDB').TestColl.insert({Key: null, X: 'Key 0', inc: 0});

    // Restart shard 0, given the fact that the ssv flag is true, then
    // mongos should send the info only once

    st.restartShardRS(0);

    // Do a scather-gather write operation. One of the shards have been restarted
    // so StaleShardVersion should be returned, and the multi-write should be
    // executed only once per shard
    var bulkOp = st.s0.getDB('TestDB').TestColl.initializeUnorderedBulkOp();
    bulkOp.find({X: 'Key 0'}).update({$inc: {inc: 1}});
    bulkOp.execute();

    // Make sure inc have been incremented only 1 time
    assert.eq(1, st.s0.getDB('TestDB').TestColl.findOne({X: 'Key 0'}).inc);
    st.stop();
})();
