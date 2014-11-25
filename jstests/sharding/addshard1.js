s = new ShardingTest( "add_shard1", 1 );

assert.eq( 1, s.config.shards.count(), "initial server count wrong" );

// create a shard and add a database; if the database is not duplicated the mongod should accepted
// it as shard
conn1 = startMongodTest( 29000 );

db1 = conn1.getDB( "testDB" );
numObjs = 0;
for (i=0; i<3; i++){
    db1.foo.save( { a : i } );
    numObjs++;
}
db1.getLastError()

newShard = "myShard";
assert( s.admin.runCommand( { addshard: "localhost:29000" , name: newShard } ).ok, "did not accepted non-duplicated shard" );

// a mongod with an existing database name should not be allowed to become a shard
conn2 = startMongodTest( 29001 );
db2 = conn2.getDB( "otherDB" );
db2.foo.save( {a:1} );
db2.getLastError()
db3 = conn2.getDB( "testDB" );
db3.foo.save( {a:1} );
db3.getLastError()

s.config.databases.find().forEach( printjson )
rejectedShard = "rejectedShard";
assert( ! s.admin.runCommand( { addshard: "localhost:29001" , name : rejectedShard } ).ok, "accepted mongod with duplicate db" );

// check that all collection that were local to the mongod's are accessible through the mongos
sdb1 = s.getDB( "testDB" );
assert.eq( numObjs , sdb1.foo.count() , "wrong count for database that existed before addshard" );
sdb2 = s.getDB( "otherDB" );
assert.eq( 0 , sdb2.foo.count() , "database of rejected shard appears through mongos" );

// make sure we can move a DB from the original mongod to a previoulsy existing shard
assert.eq( s.normalize( s.config.databases.findOne( { _id : "testDB" } ).primary ), newShard , "DB primary is wrong" );
origShard = s.getNonPrimaries( "testDB" )[0];
s.adminCommand( { moveprimary : "testDB" , to : origShard } );
assert.eq( s.normalize( s.config.databases.findOne( { _id : "testDB" } ).primary ), origShard , "DB primary didn't move" );
assert.eq( numObjs , sdb1.foo.count() , "wrong count after moving datbase that existed before addshard" );

// make sure we can shard the original collections
sdb1.foo.ensureIndex( { a : 1 }, { unique : true } ) // can't shard populated collection without an index
s.adminCommand( { enablesharding : "testDB" } );
s.adminCommand( { shardcollection : "testDB.foo" , key: { a : 1 } } );
s.adminCommand( { split : "testDB.foo", middle: { a : Math.floor(numObjs/2) } } );
assert.eq( 2 , s.config.chunks.count(), "wrong chunk number after splitting collection that existed before" );
assert.eq( numObjs , sdb1.foo.count() , "wrong count after splitting collection that existed before" );

stopMongod( 29000 );
stopMongod( 29001 );
s.stop();
