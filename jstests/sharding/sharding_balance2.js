/**
 * Test the maxSize setting for the addShard command.
 */

(function() {
"use strict";

var MaxSizeMB = 1;

var s = new ShardingTest({ shards: 2, other: { chunkSize: 1, manualAddShard: true }});
var db = s.getDB( "test" );
s.stopBalancer();

var names = s.getConnNames();
assert.eq(2, names.length);
s.adminCommand({ addshard: names[0] });
s.adminCommand({ addshard: names[1], maxSize: MaxSizeMB });

s.adminCommand({ enablesharding: "test" });
var res = db.adminCommand({ movePrimary: 'test', to: names[0] });
assert(res.ok || res.errmsg == "it is already the primary");


var bigString = "";
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

var inserted = 0;
var num = 0;
var bulk = db.foo.initializeUnorderedBulkOp();
while ( inserted < ( 40 * 1024 * 1024 ) ){
    bulk.insert({ _id: num++, s: bigString });
    inserted += bigString.length;
}
assert.writeOK(bulk.execute());
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.gt(s.config.chunks.count(), 10);

var getShardSize = function(conn) {
    var listDatabases = conn.getDB('admin').runCommand({ listDatabases: 1 });
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
        localBulk.insert({ x: x, val: bigString });
    }
    assert.writeOK(localBulk.execute());

    // Force the storage engine to flush files to disk so shardSize will get updated.
    assert.commandWorked(shardConn.getDB('admin').runCommand({ fsync: 1 }));
}

var configDB = s.s.getDB('config');
var balanceRoundsBefore = configDB.actionlog.find({ what: 'balancer.round' }).count();

s.startBalancer();

// Wait until a balancer finishes at least one round.
assert.soon(function() {
    var currentBalanceRound = configDB.actionlog.find({ what: 'balancer.round' }).count();
    return balanceRoundsBefore < currentBalanceRound;
});

var chunkCounts = s.chunkCounts('foo', 'test');
assert.eq(0, chunkCounts.shard0001);

s.stop();

})();
