

s = new ShardingTest( "rs1" , 2 /* numShards */, 1 /* verboseLevel */, 1 /* numMongos */, { rs : true , numReplicas : 2 , chunksize : 1 , nopreallocj : true } )
s.config.settings.update( { _id: "balancer" }, { $set : { stopped: true, _nosleep: true, _secondaryThrottle : true } } , true );


db = s.getDB( "test" );
for ( i=0; i<2100; i++ ) {
    db.foo.insert( { _id : i , x : i } );
}
db.getLastError();

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



