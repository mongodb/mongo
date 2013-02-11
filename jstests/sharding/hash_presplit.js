// Test that sharding a new collection with a hashed shard key pre-splits chunk ranges and
// distributes initial chunks evenly.

var s = new ShardingTest( { name : jsTestName() , shards : 3 , mongos : 1, verbose : 1 } );
var dbname = "test";
var coll = "foo";
var db = s.getDB( dbname );
db.adminCommand( { enablesharding : dbname } );

//for simplicity turn off balancer
s.stopBalancer();


// Test 1
// Using hashed shard key, but collection is non-empty, so should not pre-split.
print("****** Test 1 *******");

db.getCollection( coll ).drop();
db.getCollection( coll ).insert( { a : 1 } );

db.getCollection( coll ).ensureIndex( { a: "hashed"} );
var res = db.adminCommand( { shardcollection : dbname + "." + coll , key : { a : "hashed" } } );
assert.eq( res.ok , 1 , "shardcollection didn't work" );
db.printShardingStatus();
var numChunks = s.config.chunks.count();
assert.eq( numChunks , 1  , "sharding non-empty collection should not pre-split" );


// Test 2
// Using hashed shard key, collection is empty, and numInitialChunks specified.
print("****** Test 2 *******");

db.getCollection( coll ).drop();

var res = db.adminCommand( { shardcollection : dbname + "." + coll ,
                             key : { a : "hashed" } ,
                             numInitialChunks : 500 } );
assert.eq( res.ok , 1 , "shardcollection didn't work" );
db.printShardingStatus();
var numChunks = s.config.chunks.count();
assert.eq( numChunks , 500  , "should be exactly 500 chunks" );

var shards = s.config.shards.find();
shards.forEach(
    // check that each shard has one third the numInitialChunks
    function( shard ){
        var numChunksOnShard = s.config.chunks.find( {"shard" : shard._id} ).count();
        assert.gte( numChunksOnShard , Math.floor( 500/3 ) );
    }
);


// Test 3
// Using hashed shard key, collection is empty, and using default numInitialChunks.
print("****** Test 3 *******");

db.getCollection( coll ).drop();

// create an unrelated index. later check that pre-splitting creates index on shards with chunks
db.getCollection( coll ).ensureIndex( { dummy : 1 } );

var res = db.adminCommand( { shardcollection : dbname + "." + coll , key : { a : "hashed" } } );
assert.eq( res.ok , 1 , "shardcollection didn't work" );
db.printShardingStatus();
var numChunks = s.config.chunks.count();
assert.gt( numChunks , 1  , "should be multiple chunks" );

var shards = s.config.shards.find();
shards.forEach(
    function( shard ){
        // check that each shard has one third of the chunks
        var numChunksOnShard = s.config.chunks.find( { "shard" : shard._id } ).count();
        assert.gte( numChunksOnShard , Math.floor( numChunks/3 ) );

        // and the unrelated index
        var conn = new Mongo ( shard.host );
        printjson( conn.getDB( dbname ).getCollection( coll ).getIndexes() );
        assert.eq( 1 , conn.getDB( dbname ).system.indexes.find( { key : { dummy : 1 } } ).count(),
                   "shard missing dummy index" );
    }
);

// finally, check that the collection gets dropped correctly (which doesn't happen if pre-splitting
// fails to create the collection on all shards).
var res = db.runCommand( { "drop" : coll } );
assert.eq( res.ok , 1 , "couldn't drop empty, pre-split collection");

s.stop();

(function() {
    jsTest.log('Test hashed presplit with 1 shard.');
    var st = new ShardingTest({ shards: 1 });
    var testDB = st.getDB('test');

    //create hashed shard key and enable sharding
    testDB.adminCommand({ enablesharding: "test" });
    testDB.adminCommand({ shardCollection: "test.collection", key: { a: "hashed" }});

    //check the number of initial chunks.
    assert.eq(2, st.getDB('config').chunks.count(),
        'Using hashed shard key but failing to do correct presplitting');
    st.stop();
})();

