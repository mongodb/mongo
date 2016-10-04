//
// Tests that we can dump collection metadata via getShardVersion()
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s0;
    var coll = mongos.getCollection("foo.bar");
    var admin = mongos.getDB("admin");
    var shards = mongos.getCollection("config.shards").find().toArray();
    var shardAdmin = st.shard0.getDB("admin");

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    st.ensurePrimaryShard(coll.getDB() + "", shards[0]._id);
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

    assert.commandWorked(shardAdmin.runCommand({getShardVersion: coll + ""}));

    // Make sure we have chunks information on the shard after the shard collection call
    var result = assert.commandWorked(
        shardAdmin.runCommand({getShardVersion: coll + "", fullMetadata: true}));
    printjson(result);
    var metadata = result.metadata;

    assert.eq(metadata.chunks.length, 1);
    assert.eq(metadata.pending.length, 0);
    assert(metadata.chunks[0][0]._id + "" == MinKey + "");
    assert(metadata.chunks[0][1]._id + "" == MaxKey + "");
    assert(metadata.shardVersion + "" == result.global + "");

    // Make sure a collection with no metadata still returns the metadata field
    assert(shardAdmin.runCommand({getShardVersion: coll + "xyz", fullMetadata: true}).metadata !=
           undefined);

    // Make sure we get multiple chunks after a split
    assert(admin.runCommand({split: coll + "", middle: {_id: 0}}).ok);

    assert(shardAdmin.runCommand({getShardVersion: coll + ""}).ok);
    printjson(shardAdmin.runCommand({getShardVersion: coll + "", fullMetadata: true}));

    // Make sure we have chunks info
    result = shardAdmin.runCommand({getShardVersion: coll + "", fullMetadata: true});
    metadata = result.metadata;

    assert.eq(metadata.chunks.length, 2);
    assert.eq(metadata.pending.length, 0);
    assert(metadata.chunks[0][0]._id + "" == MinKey + "");
    assert(metadata.chunks[0][1]._id == 0);
    assert(metadata.chunks[1][0]._id == 0);
    assert(metadata.chunks[1][1]._id + "" == MaxKey + "");
    assert(metadata.shardVersion + "" == result.global + "");

    st.stop();

})();
