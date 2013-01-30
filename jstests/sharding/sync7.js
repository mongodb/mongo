// Test that the clock skew of the distributed lock disallows getting locks for moving and splitting.

s = new ShardingTest( "moveDistLock", 3, 0, undefined, { sync : true } );

s._connections[0].getDB( "admin" ).runCommand( { _skewClockCommand : 1, skew : 15000 } )
s._connections[1].getDB( "admin" ).runCommand( { _skewClockCommand : 1, skew : -16000 } )

// We need to start another mongos after skewing the clock, since the first mongos will have already
// tested the config servers (via the balancer) before we manually skewed them
otherMongos = startMongos( { port : 30020, v : 0, configdb : s._configDB } );

// Initialize DB data
initDB = function(name) {
	var db = s.getDB( name );
	var c = db.foo;
	c.save( { a : 1 } );
	c.save( { a : 2 } );
	c.save( { a : 3 } );
	assert( 3, c.count() );

	return s.getServer( name );
}

from = initDB( "test1" );
to = s.getAnother( from );

s.printShardingStatus();

// Make sure we can't move when our clock skew is so high
result = otherMongos.getDB( "admin" ).runCommand( { moveprimary : "test1", to : to.name } );
printjson(result);
s.printShardingStatus();
assert.eq( result.ok, 0, "Move command should not have succeeded!" )

// Enable sharding on DB and collection
result = otherMongos.getDB("admin").runCommand( { enablesharding : "test1" } );
result = otherMongos.getDB("test1").foo.ensureIndex( { a : 1 } );
result = otherMongos.getDB("admin").runCommand( { shardcollection : "test1.foo", key : { a : 1 } } );
print("  Collection Sharded! ")

// Make sure we can't split when our clock skew is so high
result = otherMongos.getDB( "admin" ).runCommand( { split : "test1.foo", find : { a : 2 } } );
printjson(result);
assert.eq( result.ok, 0, "Split command should not have succeeded!")

// Adjust clock back in bounds
s._connections[1].getDB( "admin" ).runCommand( { _skewClockCommand : 1, skew : 0 } )
print("  Clock adjusted back to in-bounds. ");

// Make sure we can now split
result = otherMongos.getDB( "admin" ).runCommand( { split : "test1.foo", find : { a : 2 } } );
s.printShardingStatus();
printjson(result);
assert.eq( result.ok, 1, "Split command should have succeeded!")

// Make sure we can now move
result = otherMongos.getDB( "admin" ).runCommand( { moveprimary : "test1", to : to.name } );
s.printShardingStatus();
printjson(result);
assert.eq( result.ok, 1, "Move command should have succeeded!" )

// Make sure we can now move again (getting the lock twice)
result = otherMongos.getDB( "admin" ).runCommand( { moveprimary : "test1", to : from.name } );
s.printShardingStatus();
printjson(result);
assert.eq( result.ok, 1, "Move command should have succeeded again!" )

s.stop();
