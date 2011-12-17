s = new ShardingTest( "bigMapReduce" , 2 , 1 , 1 , { chunksize : 1 } );

// reduce chunk size to split
var config = s.getDB("config");
config.settings.save({_id: "chunksize", value: 1});

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo", key : { "_id" : 1 } } )

db = s.getDB( "test" );
var str=""
for (i=0;i<4*1024;i++) { str=str+"a"; }
for (j=0; j<100; j++) for (i=0; i<512; i++){ db.foo.save({y:str})}
db.getLastError();

assert.eq( 51200, db.foo.find().itcount(), "Not all data was saved!" )

s.printChunks();
s.printChangeLog();

function map() { emit('count', 1); } 
function reduce(key, values) { return Array.sum(values) } 

// Test basic mapReduce
for ( iter=0; iter<1; iter++ ){
    out = db.foo.mapReduce(map, reduce,"big_out") 
}

// test output to a different DB
// do it multiple times so that primary shard changes
for (iter = 0; iter < 1; iter++) {
    
    assert.eq( 51200, db.foo.find().itcount(), "Not all data was found!" )
    
    outCollStr = "mr_replace_col_" + iter;
    outDbStr = "mr_db_" + iter;

    print("Testing mr replace into DB " + iter)

    res = db.foo.mapReduce( map , reduce , { out : { replace: outCollStr, db: outDbStr } } )
    printjson(res);

    outDb = s.getDB(outDbStr);
    outColl = outDb[outCollStr];

    obj = outColl.convertToSingleObject("value");
    
    assert.eq( 51200 , obj.count , "Received wrong result " + obj.count );

    print("checking result field");
    assert.eq(res.result.collection, outCollStr, "Wrong collection " + res.result.collection);
    assert.eq(res.result.db, outDbStr, "Wrong db " + res.result.db);
}

// sharded output
function map2() { emit(this._id, {count: 1, y: this.y}); }
function reduce2(key, values) { return values[0]; }

for ( iter=0; iter<1; iter++ ){

    res = db.foo.mapReduce(map2, reduce, { out : { replace: "mrShardedOut", sharded: true }});
    assert.eq( 51200 , res.counts.output , "Output is wrong " );
    printjson(res);

    outColl = db["mrShardedOut"];
    // SERVER-3645 -can't use count()
    assert.eq( 51200 , outColl.find().itcount() , "Received wrong result, this may happen intermittently until resolution of SERVER-3627" );
    // make sure it's sharded and split
    print("Number of chunks: " + config.chunks.count({ns: db.mrShardedOut._fullName}));
    assert.gt( config.chunks.count({ns: db.mrShardedOut._fullName}), 1, "didnt split");
    
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
        // should be about 200 chunks each shard
        if (Math.abs(count - shardChunks[prop]) > 8)
            assert(false, "Chunks are not well balanced: " + count + " vs " + shardChunks[prop]);
    }
}

s.stop()

