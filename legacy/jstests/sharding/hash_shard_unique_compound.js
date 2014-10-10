// Basic test of sharding with a hashed shard key and other unique index
// Does 2 things and checks for consistent error:
//  1.) shard collection on hashed "a", ensure unique index {a:1, b:1}
//  2.) reverse order
// @tags : [ hashed ]

var s = new ShardingTest( { name : jsTestName() , shards : 1 , mongos : 1, verbose : 1 } );
var dbName = "test";
var collName = "foo";
var ns = dbName + "." + collName;
var db = s.getDB( dbName );
var coll = db.getCollection( collName );

// Enable sharding on DB
var res = db.adminCommand( { enablesharding : dbName } );

// for simplicity start by turning off balancer
var res = s.stopBalancer();

// shard a fresh collection using a hashed shard key
coll.drop();
assert.commandWorked(db.adminCommand( { shardcollection : ns , key : { a : "hashed" } } ));
db.printShardingStatus();

// Create unique index
assert.commandWorked(coll.ensureIndex({ a: 1, b: 1 }, { unique: true }));

jsTest.log("------ indexes -------")
jsTest.log(tojson(coll.getIndexes()));

// Second Part
jsTest.log("------ dropping sharded collection to start part 2 -------")
coll.drop();

//Create unique index
assert.commandWorked(coll.ensureIndex({ a: 1, b: 1 }, { unique: true }));

// shard a fresh collection using a hashed shard key
assert.commandWorked(db.adminCommand( { shardcollection : ns , key : { a : "hashed" } } ),
                     "shardcollection didn't worked 2");

db.printShardingStatus();
jsTest.log("------ indexes 2-------");
jsTest.log(tojson(coll.getIndexes()));

s.stop()
