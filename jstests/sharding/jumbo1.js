(function() {
'use strict';

var s = new ShardingTest({shards: 2, other: {chunkSize: 1}});

assert.commandWorked(s.s.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', s.shard1.shardName);
assert.commandWorked(
    s.s.adminCommand({addShardToZone: s.shard0.shardName, zone: 'finalDestination'}));

// Set the chunk range with a zone that will cause the chunk to be in the wrong place so the
// balancer will be forced to attempt to move it out.
assert.commandWorked(s.s.adminCommand({shardcollection: "test.foo", key: {x: 1}}));
assert.commandWorked(s.s.adminCommand(
    {updateZoneKeyRange: 'test.foo', min: {x: 0}, max: {x: MaxKey}, zone: 'finalDestination'}));

var db = s.getDB("test");

const big = 'X'.repeat(10000);

// Create sufficient documents to create a jumbo chunk, and use the same shard key in all of
// them so that the chunk cannot be split.
var bulk = db.foo.initializeUnorderedBulkOp();
for (var i = 0; i < 200; i++) {
    bulk.insert({x: 0, big: big});
}

assert.commandWorked(bulk.execute());

s.startBalancer();

// Wait for the balancer to try to move the chunk and mark it as jumbo.
assert.soon(() => {
    let chunk = s.getDB('config').chunks.findOne({ns: 'test.foo', min: {x: 0}});
    if (chunk == null) {
        // Balancer hasn't run and enforce the zone boundaries yet.
        return false;
    }

    assert.eq(s.shard1.shardName, chunk.shard, `${tojson(chunk)} was moved by the balancer`);
    return chunk.jumbo;
});

s.stop();
})();
