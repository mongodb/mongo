s = new ShardingTest( "add_shard1", 1 );

assert.eq( 1, s.config.shards.count(), "initial server count wrong" );

// create a shard and add a database; a non-empty mongod should not be accepted as a shard
conn = startMongodTest( 29000 );
db = conn.getDB( "test" );
db.foo.save( {a:1} );
assert( ! s.admin.runCommand( { addshard: "localhost:29000" } ).ok, "accepted non-empty shard" );

stopMongod( 29000 );
s.stop();