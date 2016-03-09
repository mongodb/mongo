//
// Tests that merging chunks via mongos works/doesn't work with different chunk configurations
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 2});

    var mongos = st.s0;
    var staleMongos = st.s1;
    var admin = mongos.getDB("admin");
    var shards = mongos.getCollection("config.shards").find().toArray();
    var coll = mongos.getCollection("foo.bar");

    assert(admin.runCommand({enableSharding: coll.getDB() + ""}).ok);
    printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: shards[0]._id}));
    assert(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}).ok);

    // Create ranges MIN->0,0->10,(hole),20->40,40->50,50->90,(hole),100->110,110->MAX on first
    // shard
    jsTest.log("Creating ranges...");

    assert(admin.runCommand({split: coll + "", middle: {_id: 0}}).ok);
    assert(admin.runCommand({split: coll + "", middle: {_id: 10}}).ok);
    assert(admin.runCommand({split: coll + "", middle: {_id: 20}}).ok);
    assert(admin.runCommand({split: coll + "", middle: {_id: 40}}).ok);
    assert(admin.runCommand({split: coll + "", middle: {_id: 50}}).ok);
    assert(admin.runCommand({split: coll + "", middle: {_id: 90}}).ok);
    assert(admin.runCommand({split: coll + "", middle: {_id: 100}}).ok);
    assert(admin.runCommand({split: coll + "", middle: {_id: 110}}).ok);

    assert(admin.runCommand({moveChunk: coll + "", find: {_id: 10}, to: shards[1]._id}).ok);
    assert(admin.runCommand({moveChunk: coll + "", find: {_id: 90}, to: shards[1]._id}).ok);

    st.printShardingStatus();

    // Insert some data into each of the consolidated ranges
    assert.writeOK(coll.insert({_id: 0}));
    assert.writeOK(coll.insert({_id: 40}));
    assert.writeOK(coll.insert({_id: 110}));

    var staleCollection = staleMongos.getCollection(coll + "");

    jsTest.log("Trying merges that should fail...");

    // S0: min->0, 0->10, 20->40, 40->50, 50->90, 100->110, 110->max
    // S1: 10->20, 90->100

    // Make sure merging non-exact chunks is invalid

    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 5}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 5}, {_id: 10}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 15}, {_id: 50}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 20}, {_id: 55}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 105}, {_id: MaxKey}]}).ok);

    // Make sure merging single chunks is invalid

    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 0}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 20}, {_id: 40}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 110}, {_id: MaxKey}]}).ok);

    // Make sure merging over holes is invalid

    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 0}, {_id: 40}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 40}, {_id: 110}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 50}, {_id: 110}]}).ok);

    // Make sure merging between shards is invalid

    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 0}, {_id: 20}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 10}, {_id: 40}]}).ok);
    assert(!admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 40}, {_id: 100}]}).ok);

    assert.eq(3, staleCollection.find().itcount());

    jsTest.log("Trying merges that should succeed...");

    assert(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 10}]}).ok);

    assert.eq(3, staleCollection.find().itcount());

    // S0: min->10, 20->40, 40->50, 50->90, 100->110, 110->max
    // S1: 10->20, 90->100

    // Make sure merging three chunks is valid.

    jsTest.log(tojson(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 20}, {_id: 90}]})));

    // S0: min->10, 20->90, 100->110, 110->max
    // S1: 10->20, 90->100

    assert.eq(3, staleCollection.find().itcount());

    assert(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 100}, {_id: MaxKey}]}).ok);

    assert.eq(3, staleCollection.find().itcount());

    // S0: min->10, 20->90, 100->max
    // S1: 10->20, 90->100

    st.printShardingStatus();

    st.stop();

})();
