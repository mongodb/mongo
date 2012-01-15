s = new ShardingTest( "mrShardedOutput" , 2 , 1 , 1 , { chunksize : 1 } );

// reduce chunk size to split
var config = s.getDB("config");
config.settings.save({_id: "chunksize", value: 1});

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo", key : { "a" : 1 } } )

db = s.getDB( "test" );
var aaa = "aaaaaaaaaaaaaaaa";
var str = aaa;
while (str.length < 1*1024) { str += aaa; }

s.printChunks();
s.printChangeLog();

function map2() { emit(this._id, {count: 1, y: this.y}); }
function reduce2(key, values) { return values[0]; }

var numdocs = 0;
var numbatch = 100000;
var nchunks = 0;
for ( iter=0; iter<2; iter++ ){
    // add some more data for input so that chunks will get split further
    for (i=0; i<numbatch; i++){ db.foo.save({a: Math.random() * 1000, y:str})}
    db.getLastError();
    numdocs += numbatch
    
    var isBad = db.foo.find().itcount() != numdocs
    
    // Verify that wbl weirdness isn't causing this
    assert.soon( function(){ var c = db.foo.find().itcount(); print( "Count is " + c ); return c == numdocs } )
    assert( ! isBad )
    //assert.eq( numdocs, db.foo.find().itcount(), "Not all data was saved!" )
    
    res = db.foo.mapReduce(map2, reduce2, { out : { replace: "mrShardedOut", sharded: true }});
    assert.eq( numdocs , res.counts.output , "Output is wrong " );
    printjson(res);

    outColl = db["mrShardedOut"];
    // SERVER-3645 -can't use count()
    assert.eq( numdocs , outColl.find().itcount() , "Received wrong result, this may happen intermittently until resolution of SERVER-3627" );
    // make sure it's sharded and split
    var newnchunks = config.chunks.count({ns: db.mrShardedOut._fullName});
    print("Number of chunks: " + newnchunks);
    assert.gt( newnchunks, 1, "didnt split");

    // make sure num of chunks increases over time
    if (nchunks)
        assert.gt( newnchunks, nchunks, "number of chunks did not increase between iterations");
    nchunks = newnchunks;

    // check that chunks are well distributed
    cur = config.chunks.find({ns: db.mrShardedOut._fullName});
    shardChunks = {};
    while (cur.hasNext()) {
        chunk = cur.next();
        printjson(chunk);
        sname = chunk.shard;
        if (shardChunks[sname] == undefined) shardChunks[sname] = 0;
        shardChunks[chunk.shard] += 1;
    }

    var count = 0;
    for (var prop in shardChunks) {
        print ("NUMBER OF CHUNKS FOR SHARD " + prop + ": " + shardChunks[prop]);
        if (!count)
            count = shardChunks[prop];
        assert.lt(Math.abs(count - shardChunks[prop]), nchunks / 10, "Chunks are not well balanced: " + count + " vs " + shardChunks[prop]);
    }
}


s.stop();
