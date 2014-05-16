var otherOptions = { rs: true , numReplicas: 2 , chunksize: 1 , nopreallocj: true };
var s = new ShardingTest({ shards: 2, verbose: 1, other: otherOptions });
s.config.settings.update({ _id: "balancer" },
                         { $set: { stopped: true, _secondaryThrottle: true }}, true );

db = s.getDB( "test" );
var bulk = db.foo.initializeUnorderedBulkOp();
for (var i = 0; i < 2100; i++) {
    bulk.insert({ _id: i, x: i });
}
assert.writeOK(bulk.execute());

serverName = s.getServerName( "test" ) 
other = s.config.shards.findOne( { _id : { $ne : serverName } } );

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

for ( i=0; i<20; i++ )
    s.adminCommand( { split : "test.foo" , middle : { _id : i * 100 } } );

assert.eq( 2100, db.foo.find().itcount() );
coll = db.foo;
coll.setSlaveOk();



for ( i=0; i<20; i++ ) {
    // Needs to waitForDelete because we'll be performing a slaveOk query,
    // and secondaries don't have a chunk manager so it doesn't know how to
    // filter out docs it doesn't own.
    s.adminCommand({ moveChunk: "test.foo", find: { _id: i * 100 }, to : other._id,
        _secondaryThrottle: true, _waitForDelete: true });
    assert.eq( 2100, coll.find().itcount() );
}

s.stop();



