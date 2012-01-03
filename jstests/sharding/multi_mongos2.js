// multi_mongos2.js
// This tests sharding an existing collection that both shards are aware of (SERVER-2828)


// setup sharding with two mongos, s1 and s2
s1 = new ShardingTest( "multi_mongos1" , 2 , 1 , 2 );
s2 = s1._mongos[1];

s1.adminCommand( { enablesharding : "test" } );
s1.adminCommand( { shardcollection : "test.foo" , key : { num : 1 } } );

s1.config.databases.find().forEach( printjson )

// test queries

s1.getDB('test').existing.insert({_id:1})
assert.eq(1, s1.getDB('test').existing.count({_id:1}));
assert.eq(1, s2.getDB('test').existing.count({_id:1}));

// We need to turn off the balancer before doing manual moves, otherwise they can interfere
s1.stopBalancer()

s2.adminCommand( { shardcollection : "test.existing" , key : { _id : 1 } } );
s2.adminCommand( { split : "test.existing" , find : { _id : 5 } } )

res = s2.getDB( "admin" ).runCommand( { moveChunk: "test.existing" , find : { _id : 1 } , to : s1.getOther( s1.getServer( "test" ) ).name } );

assert.eq(1 , res.ok, tojson(res));

s1.setBalancer( true )

printjson( s2.adminCommand( {"getShardVersion" : "test.existing" } ) )
printjson( new Mongo(s1.getServer( "test" ).name).getDB( "admin" ).adminCommand( {"getShardVersion" : "test.existing" } ) )

assert.eq(1, s1.getDB('test').existing.count({_id:1})); // SERVER-2828
assert.eq(1, s2.getDB('test').existing.count({_id:1}));

// test stats

s1.getDB('test').existing2.insert({_id:1})
assert.eq(1, s1.getDB('test').existing2.count({_id:1}));
assert.eq(1, s2.getDB('test').existing2.count({_id:1}));

s2.adminCommand( { shardcollection : "test.existing2" , key : { _id : 1 } } );
s2.adminCommand( { split : "test.existing2" , find : { _id : 5 } } )

var res = s1.getDB('test').existing2.stats()
printjson( res )
assert.eq(true, res.sharded); //SERVER-2828
assert.eq(true, s2.getDB('test').existing2.stats().sharded);

// test admin commands

s1.getDB('test').existing3.insert({_id:1})
assert.eq(1, s1.getDB('test').existing3.count({_id:1}));
assert.eq(1, s2.getDB('test').existing3.count({_id:1}));

s1.stopBalancer()

s2.adminCommand( { shardcollection : "test.existing3" , key : { _id : 1 } } );
s2.adminCommand( { split : "test.existing3" , find : { _id : 5 } } )

res = s1.getDB( "admin" ).runCommand( { moveChunk: "test.existing3" , find : { _id : 1 } , to : s1.getOther( s1.getServer( "test" ) ).name } );
assert.eq(1 , res.ok, tojson(res));

s1.setBalancer( true )

s1.stop();
