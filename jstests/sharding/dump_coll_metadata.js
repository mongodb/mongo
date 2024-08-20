//
// Tests that we can dump collection metadata via getShardVersion()
//
import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({shards: 2});

var coll = st.s.getCollection("foo.bar");
var shardAdmin = st.shard0.getDB("admin");

assert.commandWorked(
    st.s.adminCommand({enableSharding: coll.getDB() + "", primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: coll + "", key: {_id: 1}}));

assert.commandWorked(shardAdmin.runCommand({getShardVersion: coll + ""}));

function getCollMetadataWithRefresh(node, collName) {
    var shardVersionRes;

    assert.soon(() => {
        assert.commandWorked(node.adminCommand({_flushRoutingTableCacheUpdates: collName}));
        var res = assert.commandWorked(
            node.adminCommand({getShardVersion: collName, fullMetadata: true}));
        assert(res.hasOwnProperty('global'),
               `Did not found 'global' property in metadata object: ${tojson(res)}`);

        if (res.global === 'UKNOWN') {
            return false;
        }

        shardVersionRes = res;
        return true;
    });

    return shardVersionRes;
}

// Make sure we have chunks information on the shard after the shard collection call
var svRes = getCollMetadataWithRefresh(st.shard0, coll.getFullName());

assert.eq(svRes.metadata.chunks.length, 1);
assert.eq(svRes.metadata.chunks[0][0]._id, MinKey);
assert.eq(svRes.metadata.chunks[0][1]._id, MaxKey);
assert.eq(svRes.metadata.shardVersion, svRes.global);

// Make sure a collection with no metadata still returns the metadata field
assert.neq(shardAdmin.runCommand({getShardVersion: coll + "xyz", fullMetadata: true}).metadata,
           undefined);

// Make sure we get multiple chunks after a split and refresh -- splits by themselves do not
// cause the shard to refresh.
assert.commandWorked(st.s.adminCommand({split: coll + "", middle: {_id: 0}}));
assert.commandWorked(shardAdmin.runCommand({getShardVersion: coll + ""}));

svRes = getCollMetadataWithRefresh(st.shard0, coll.getFullName());

assert.eq(svRes.metadata.chunks.length, 2);
assert(svRes.metadata.chunks[0][0]._id + "" == MinKey + "");
assert(svRes.metadata.chunks[0][1]._id == 0);
assert(svRes.metadata.chunks[1][0]._id == 0);
assert(svRes.metadata.chunks[1][1]._id + "" == MaxKey + "");
assert(tojson(svRes.metadata.shardVersion) == tojson(svRes.global));

st.stop();
