
s = new ShardingTest( "blah" , 1 /* numShards */, 1 /* verboseLevel */, 1 /* numMongos */, { rs : true , chunksize : 1 } )

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

db = s.getDB( "test" );

for ( i=0; i<100; i++ )  {
    db.foo.insert( { _id : i , x : i } )
    db.bar.insert( { _id : i , x : i } )
}

db.getLastError();

sh.splitAt( "test.foo" , { _id : 50 } )

other = new Mongo( s.s.name );
dbother = other.getDB( "test" );

assert.eq( 5 , db.foo.findOne( { _id : 5 } ).x );
assert.eq( 5 , dbother.foo.findOne( { _id : 5 } ).x );

assert.eq( 5 , db.bar.findOne( { _id : 5 } ).x );
assert.eq( 5 , dbother.bar.findOne( { _id : 5 } ).x );


s._rs[0].test.awaitReplication();

s._rs[0].test.stopMaster( 15 , true )

// Wait for the primary to come back online...
var primary = s._rs[0].test.getPrimary();

// Wait for the mongos to recognize the new primary...
ReplSetTest.awaitRSClientHosts( db.getMongo(), primary, { ismaster : true } );

assert.eq( 5 , db.foo.findOne( { _id : 5 } ).x );
assert.eq( 5 , db.bar.findOne( { _id : 5 } ).x );

s.adminCommand( { shardcollection : "test.bar" , key : { _id : 1 } } );
sh.splitAt( "test.bar" , { _id : 50 } )

yetagain = new Mongo( s.s.name )
assert.eq( 5 , yetagain.getDB( "test" ).bar.findOne( { _id : 5 } ).x )
assert.eq( 5 , yetagain.getDB( "test" ).foo.findOne( { _id : 5 } ).x )

assert.eq( 5 , dbother.bar.findOne( { _id : 5 } ).x );
assert.eq( 5 , dbother.foo.findOne( { _id : 5 } ).x );


s.stop();

