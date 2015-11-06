// Where we test operations dealing with large chunks
(function() {

// Starts a new sharding environment limiting the chunksize to 1GB (highest value allowed).
// Note that early splitting will start with a 1/4 of max size currently.  
var s = new ShardingTest({ name: 'large_chunk',
                           shards: 2,
                           other: { chunkSize: 1024 } });

// take the balancer out of the equation
s.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true );
s.config.settings.find().forEach(printjson);

db = s.getDB( "test" );

//
// Step 1 - Test moving a large chunk
//

// Turn on sharding on the 'test.foo' collection and generate a large chunk
s.adminCommand( { enablesharding : "test" } );
s.ensurePrimaryShard('test', 'shard0001');

bigString = ""
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

inserted = 0;
num = 0;
var bulk = db.foo.initializeUnorderedBulkOp();
while ( inserted < ( 400 * 1024 * 1024 ) ){
    bulk.insert({ _id: num++, s: bigString });
    inserted += bigString.length;
}
assert.writeOK(bulk.execute());

s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

assert.eq( 1 , s.config.chunks.count() , "step 1 - need one large chunk" );

primary = s.getServer( "test" ).getDB( "test" );
secondary = s.getOther( primary ).getDB( "test" );

// Make sure that we don't move that chunk if it goes past what we consider the maximum chunk size
print("Checkpoint 1a")
max = 200 * 1024 * 1024;
assert.throws(function() {
        s.adminCommand({ movechunk: "test.foo",
                         find: { _id: 1 },
                         to: secondary.getMongo().name,
                         maxChunkSizeBytes: max });
    });

// Move the chunk
print("checkpoint 1b");
before = s.config.chunks.find().toArray();
s.adminCommand( { movechunk : "test.foo" , find : { _id : 1 } , to : secondary.getMongo().name } );
after = s.config.chunks.find().toArray();
assert.neq( before[0].shard , after[0].shard , "move chunk did not work" );

s.config.changelog.find().forEach( printjson )

s.stop();

})();
