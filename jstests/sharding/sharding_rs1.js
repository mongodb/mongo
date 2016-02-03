// tests sharding with replica sets
(function() {
'use strict';

var s = new ShardingTest({ shards: 3,
                           other: { rs: true, chunkSize: 1, enableBalancer: true }});

s.adminCommand( { enablesharding : "test" } );
s.ensurePrimaryShard('test', 'test-rs0');
s.config.settings.update( { _id: "balancer" }, { $set : { _waitForDelete : true } } , true );

var db = s.getDB("test");

var bigString = "X".repeat(256 * 1024);

var insertedBytes = 0;
var num = 0;

// Insert 10 MB of data to result in 10+ chunks
var bulk = db.foo.initializeUnorderedBulkOp();
while (insertedBytes < (10 * 1024 * 1024)) {
    bulk.insert({ _id: num++, s: bigString, x: Math.random() });
    insertedBytes += bigString.length;
}
assert.writeOK(bulk.execute({w: 3}));

assert.commandWorked(s.s.adminCommand({ shardcollection: "test.foo" , key: { _id: 1 } }));

jsTest.log("Waiting for balance to complete");
s.awaitBalance('foo', 'test', 3 * 60 * 1000);

jsTest.log("Stopping balancer");
s.stopBalancer();

jsTest.log("Balancer stopped, checking dbhashes");
s._rs.forEach(function(rsNode) {
    rsNode.test.awaitReplication();

    var dbHashes = rsNode.test.getHashes("test");
    print(rsNode.url + ': ' + tojson(dbHashes));

    for (var j = 0; j < dbHashes.slaves.length; j++) {
        assert.eq(dbHashes.master.md5,
                  dbHashes.slaves[j].md5,
                  "hashes not same for: " + rsNode.url + " slave: " + j);
    }
});

assert.eq( num , db.foo.find().count() , "C1" )
assert.eq( num , db.foo.find().itcount() , "C2" )
assert.eq( num , db.foo.find().sort( { _id : 1 } ).itcount() , "C3" )
assert.eq( num , db.foo.find().sort( { _id : -1 } ).itcount() , "C4" )

db.foo.ensureIndex( { x : 1 } );
assert.eq( num , db.foo.find().sort( { x : 1 } ).itcount() , "C5" )
assert.eq( num , db.foo.find().sort( { x : -1 } ).itcount() , "C6" )

s.stop();

})();
