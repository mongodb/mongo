/**
 * Test the maxSize setting for the addShard command.
 *
 * @tags: [does_not_support_stepdowns]
 */
(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");
load("jstests/libs/feature_flag_util.js");

var MaxSizeMB = 1;

var s = new ShardingTest({
    shards: 2,
    other: {
        chunkSize: 1,
        manualAddShard: true,
        shardOptions:
            {setParameter: {internalQueryMaxBlockingSortMemoryUsageBytes: 32 * 1024 * 1024}}
    }
});

var db = s.getDB("test");

var names = s.getConnNames();
assert.eq(2, names.length);
assert.commandWorked(s.s0.adminCommand({addshard: names[0]}));
assert.commandWorked(s.s0.adminCommand({addshard: names[1], maxSize: MaxSizeMB}));
assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', names[0]);

var bigString = "";
while (bigString.length < 10000)
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

var inserted = 0;
var num = 0;
var bulk = db.foo.initializeUnorderedBulkOp();
while (inserted < (40 * 1024 * 1024)) {
    bulk.insert({_id: num++, s: bigString});
    inserted += bigString.length;
}
assert.commandWorked(bulk.execute());

assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {_id: 1}}));

var getShardSize = function(conn) {
    var listDatabases = conn.getDB('admin').runCommand({listDatabases: 1});
    return listDatabases.totalSize;
};

var shardConn = new Mongo(names[1]);

// Make sure that shard doesn't have any documents.
assert.eq(0, shardConn.getDB('test').foo.find().itcount());

var maxSizeBytes = MaxSizeMB * 1024 * 1024;

// Fill the shard with documents to exceed the max size so the balancer won't move
// chunks to this shard.
var localColl = shardConn.getDB('local').padding;
while (getShardSize(shardConn) < maxSizeBytes) {
    var localBulk = localColl.initializeUnorderedBulkOp();

    for (var x = 0; x < 20; x++) {
        localBulk.insert({x: x, val: bigString});
    }
    assert.commandWorked(localBulk.execute());

    // Force the storage engine to flush files to disk so shardSize will get updated.
    assert.commandWorked(shardConn.getDB('admin').runCommand({fsync: 1}));
}

s.startBalancer();
s.awaitBalancerRound();

var chunkCounts = s.chunkCounts('foo', 'test');
assert.eq(0, chunkCounts[s.rs1.name]);

s.stop();
})();
