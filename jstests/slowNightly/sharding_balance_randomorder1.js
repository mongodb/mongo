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
    db.foo.insert( { _id : Math.random() , s : bigString } );
    inserted += bigString.length;
}

db.getLastError();
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.lt( 20 , s.config.chunks.count()  , "setup2" );

function diff(){
    var x = s.chunkCounts( "foo" );
    printjson( x )
    return Math.max( x.shard0000 , x.shard0001 ) - Math.min( x.shard0000 , x.shard0001 );
}

function sum(){
    var x = s.chunkCounts( "foo" );
    return x.shard0000 + x.shard0001;
}

assert.lt( 20 , diff() , "big differential here" );
print( diff() )

assert.soon( function(){
    var d = diff();
    return d < 5;
} , "balance didn't happen" , 1000 * 60 * 3 , 5000 );
    
var chunkCount = sum();
s.adminCommand( { removeshard: "shard0000" } );

assert.soon( function(){
    printjson(s.chunkCounts( "foo" ));
    s.config.shards.find().forEach(function(z){printjson(z);});
    return chunkCount == s.config.chunks.count({shard: "shard0001"});
} , "removeshard didn't happen" , 1000 * 60 * 3 , 5000 );

s.stop();
