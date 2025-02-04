/**
 * Basic checks on the consistency of the 'listShards' and 'getShardMap' command responses.
 */

const listShardsResponse = assert.commandWorked(db.adminCommand({listShards: 1}));
const getShardMapResponse = assert.commandWorked(db.adminCommand({getShardMap: 1}));

jsTest.log('Check that getShardMap returns consistent information across its sections...');
// NOTE: The config server is always included using 'config' as its shard ID, even when it doesn't
// act as a regular shard.
assert.eq(getShardMapResponse.map.length, getShardMapResponse.hosts.length);
assert.eq(getShardMapResponse.map.length, getShardMapResponse.connStrings.length);
for (const shardId in getShardMapResponse.map) {
    const replSetConnString = getShardMapResponse.map[shardId];
    assert.eq(shardId, getShardMapResponse.connStrings[replSetConnString]);
    const replSetHostList = replSetConnString.slice(replSetConnString.indexOf('/') + 1);
    for (let replSetHost of replSetHostList.split(',')) {
        assert.eq(shardId, getShardMapResponse.hosts[replSetHost]);
    }
}

jsTest.log('Check that listShards returns information consistent with getShardMap...');
// In this case, 'config' only appears when a config shard is present.
const numEntriesInGetShardMapResponse = Object.keys(getShardMapResponse.map).length;
assert(numEntriesInGetShardMapResponse === listShardsResponse.shards.length + 1 ||
       numEntriesInGetShardMapResponse === listShardsResponse.shards.length);

for (let shardInfo of listShardsResponse.shards) {
    assert.eq(shardInfo.host, getShardMapResponse.map[shardInfo._id]);
}
