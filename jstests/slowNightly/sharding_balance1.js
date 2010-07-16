// sharding_balance1.js


s = new ShardingTest( "slow_sharding_balance1" , 2 , 2 , 1 , { chunksize : 1 } )

s.adminCommand( { enablesharding : "test" } );

s.config.settings.find().forEach( printjson )

db = s.getDB( "test" );

bigString = ""
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

inserted = 0;
num = 0;
while ( inserted < ( 20 * 1024 * 1024 ) ){
    db.foo.insert( { _id : num++ , s : bigString } );
    inserted += bigString.length;
}

db.getLastError();
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.lt( 20 , s.config.chunks.count()  , "setup2" );

function diff(){
    var x = s.chunkCounts( "foo" );
    printjson( x )
    return Math.max( x.shard0 , x.shard1 ) - Math.min( x.shard0 , x.shard1 );
}

function sum(){
    var x = s.chunkCounts( "foo" );
    return x.shard0 + x.shard1;
}

assert.lt( 20 , diff() , "big differential here" );
print( diff() )

assert.soon( function(){
    var d = diff();
    return d < 5;
} , "balance didn't happen" , 1000 * 60 * 3 , 5000 );
    
var chunkCount = sum();
host = s.config.shards.findOne({_id : "shard0" }).host;
s.adminCommand( { removeshard: host } );

assert.soon( function(){
    printjson(s.chunkCounts( "foo" ));
    s.config.shards.find().forEach(function(z){printjson(z);});
    return chunkCount == s.config.chunks.count({shard: "shard1"});
} , "removeshard didn't happen" , 1000 * 60 * 3 , 5000 );

s.stop();
