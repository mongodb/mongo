(function() {
"use strict";

function shardKnowledgeIsShardedOrUnknown(shard, nss) {
    let res = assert.commandWorked(shard.adminCommand({getShardVersion: nss, fullMetadata: true}));
    return (typeof res.global == 'string' && res.global == 'UNKNOWN') ||
        (typeof res.metadata == 'object' && typeof res.metadata.collVersion != 'undefined');
}

const st = new ShardingTest({shards: 2, mongos: 1});

void function testOptimizedShardCollection() {
    const dbName = 'testDB1';
    const collName = 'testColl1';

    jsTest.log("Testing that implicit collection creation triggered by optimized " +
               "shardCollection leaves all shards with the expected knowledge");

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: 'hashed'}}));

    assert(shardKnowledgeIsShardedOrUnknown(st.shard0, `${dbName}.${collName}`),
           "Unexpected sharding state in Shard 0");
    assert(shardKnowledgeIsShardedOrUnknown(st.shard1, `${dbName}.${collName}`),
           "Unexpected sharding state in Shard 1");
}();

void function testmovePrimary() {
    const dbName = 'testDB2';
    const collName = 'testColl2';

    jsTest.log("Testing that implicit collection creation triggered by movePrimary " +
               "leaves all shards with the expected knowledge");

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

    assert.commandWorked(
        st.s.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: 1}}));

    assert.commandWorked(st.s.adminCommand({
        movePrimary: dbName,
        to: st.shard1.name,
    }));

    assert(shardKnowledgeIsShardedOrUnknown(st.shard0, `${dbName}.${collName}`),
           "Unexpected sharding state in Shard 0");
    assert(shardKnowledgeIsShardedOrUnknown(st.shard1, `${dbName}.${collName}`),
           "Unexpected sharding state in Shard 1");
}();

st.stop();
})();