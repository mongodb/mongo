s = new ShardingTest( "add_shard1", 1 );

assert.eq( 1, s.config.shards.count(), "initial server count wrong" );

// create a shard and add a database; if the database is not duplicated the mongod should accepted
// it as shard
conn1 = startMongodTest( 29000 );
db1 = conn1.getDB( "testDB" );
db1.foo.save( {a:1} );
db1.getLastError()
assert( s.admin.runCommand( { addshard: "localhost:29000" } ).ok, "did not accepted non-duplicated shard" );

// a mongod with an existing database name should not be allowed to become a shard
conn2 = startMongodTest( 29001 );
db2 = conn2.getDB( "otherDB" );
db2.foo.save( {a:1} );
db2.getLastError()
db3 = conn2.getDB( "testDB" );
db3.foo.save( {a:1} );
db3.getLastError()
s.config.databases.find().forEach( printjson )
assert( ! s.admin.runCommand( { addshard: "localhost:29001" } ).ok, "accepted mongod with duplicate db" );

// check that all collection that were local to the mongod's are accessible through the mongos
sdb1 = s.getDB( "testDB" );
assert.eq( 1 , sdb1.foo.count() , "wrong count for database that existed before addshard" );
sdb2 = s.getDB( "otherDBxx" );
assert.eq( 0 , sdb2.foo.count() , "database of rejected shard appears through mongos" );

stopMongod( 29000 );
stopMongod( 29001 );
s.stop();
