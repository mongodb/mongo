//
// Tests that merge, split and move chunks via mongos works/doesn't work with different chunk
// configurations
//
(function() {
    'use strict';

    /**
     * Performs a find() on config.chunks on 'configDB', targeting chunks for the collection 'ns',
     * and the optional 'extraQuery' and 'projection'.
     * Chooses to query chunks by their 'ns' or uuid' fields according to it's config.collection
     * entry having 'timestamp' or not.
     */
    let findChunksByNs = function(configDB, ns, extraQuery = null, projection = null) {
        const chunksQuery = Object.assign({ns: ns}, extraQuery);
        return configDB.chunks.find(chunksQuery, projection);
    };

    var st = new ShardingTest({shards: 2, mongos: 2});

    var mongos = st.s0;
    var staleMongos = st.s1;
    var admin = mongos.getDB("admin");
    var coll = mongos.getCollection("foo.bar");

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    st.ensurePrimaryShard('foo', st.shard0.shardName);
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

    // Create ranges MIN->0,0->10,(hole),20->40,40->50,50->90,(hole),100->110,110->MAX on first
    // shard
    jsTest.log("Creating ranges...");

    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 10}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 20}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 40}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 50}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 90}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 100}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 110}}));

    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 10}, to: st.shard1.shardName}));
    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 90}, to: st.shard1.shardName}));

    st.printShardingStatus();

    // Insert some data into each of the consolidated ranges
    let numDocs = 0;
    for (let i = 120; i <= 240; i++) {
        assert.commandWorked(coll.insert({_id: i}));
        numDocs++;
    }

    var staleCollection = staleMongos.getCollection(coll + "");

    // S0: min->0, 0->10, 20->40, 40->50, 50->90, 100->110, 110->max
    // S1: 10->20, 90->100
    assert.eq(9, findChunksByNs(st.s0.getDB('config'), 'foo.bar').itcount());

    jsTest.log("Trying merges that should succeed...");

    // Make sure merge including the MinKey works
    assert.commandWorked(
        admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 10}]}));
    assert.eq(8, findChunksByNs(st.s0.getDB('config'), 'foo.bar').itcount());
    assert.eq(numDocs, staleCollection.find().itcount());
    // S0: min->10, 20->40, 40->50, 50->90, 100->110, 110->max
    // S1: 10->20, 90->100

    // Make sure merging three chunks in the middle works
    assert.commandWorked(
        admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 20}, {_id: 90}]}));
    assert.eq(6, findChunksByNs(st.s0.getDB('config'), 'foo.bar').itcount());
    assert.eq(numDocs, staleCollection.find().itcount());
    // S0: min->10, 20->90, 100->110, 110->max
    // S1: 10->20, 90->100

    // Make sure splitting chunks after merging works
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 55}}));
    assert.eq(7, findChunksByNs(st.s0.getDB('config'), 'foo.bar').itcount());
    assert.eq(numDocs, staleCollection.find().itcount());
    // S0: min->10, 20->55, 55->90, 100->110, 110->max
    // S1: 10->20, 90->100

    // make sure moving the new chunk works
    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 20}, to: st.shard1.shardName}));
    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 55}, to: st.shard1.shardName}));
    assert.eq(7, findChunksByNs(st.s0.getDB('config'), 'foo.bar').itcount());
    assert.eq(numDocs, staleCollection.find().itcount());
    // S0: min->10, 100->110, 110->max
    // S1: 10->20, 20->55, 55->90, 90->100

    // Make sure merge including the MaxKey works
    assert.commandWorked(
        admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 100}, {_id: MaxKey}]}));
    assert.eq(numDocs, staleCollection.find().itcount());
    assert.eq(6, findChunksByNs(st.s0.getDB('config'), 'foo.bar').itcount());
    // S0: min->10, 100->max
    // S1: 10->20, 20->55, 55->90, 90->100

    // Make sure merging chunks after a chunk has been moved out of a shard succeeds
    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 110}, to: st.shard1.shardName}));
    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 10}, to: st.shard0.shardName}));
    assert.eq(numDocs, staleCollection.find().itcount());
    assert.eq(6, findChunksByNs(st.s0.getDB('config'), 'foo.bar').itcount());
    // S0: min->10, 10->20
    // S1: 20->55, 55->90, 90->100, 100->max

    assert.commandWorked(
        admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 90}, {_id: MaxKey}]}));
    assert.commandWorked(
        admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 20}, {_id: 90}]}));
    assert.eq(numDocs, staleCollection.find().itcount());
    // S0: min->10, 10->20
    // S1: 20->90, 90->max

    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 15}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 30}}));
    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 30}, to: st.shard0.shardName}));
    assert.eq(numDocs, staleCollection.find().itcount());
    // S0: min->10, 10->15, 15->20, 30->90
    // S1: 20->30, 90->max

    // range has ha hole on shard 0
    assert.commandFailed(
        admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 90}]}));

    // Make sure merge on the other shard after a chunk has been merged succeeds
    assert.commandWorked(
        admin.runCommand({moveChunk: coll + "", find: {_id: 20}, to: st.shard0.shardName}));
    assert.commandWorked(
        admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 90}]}));
    // S0: min->90
    // S1: 90->max

    st.printShardingStatus(true);

    assert.eq(2, findChunksByNs(st.s0.getDB('config'), 'foo.bar').itcount());
    assert.eq(1, findChunksByNs(st.s0.getDB('config'), 'foo.bar', {
                     'min._id': MinKey,
                     'max._id': 90,
                     shard: st.shard0.shardName
                 }).itcount());
    assert.eq(1, findChunksByNs(st.s0.getDB('config'), 'foo.bar', {
                     'min._id': 90,
                     'max._id': MaxKey,
                     shard: st.shard1.shardName
                 }).itcount());

    st.stop();
})();
