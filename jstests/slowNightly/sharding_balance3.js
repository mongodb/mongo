// sharding_balance3.js

// simple test to make sure things get balanced 

s = new ShardingTest( "slow_sharding_balance3" , 2 , 3 , 1 , { chunksize : 1 } );

s.adminCommand( { enablesharding : "test" } );

s.config.settings.find().forEach( printjson );

db = s.getDB( "test" );

bigString = ""
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

inserted = 0;
num = 0;
while ( inserted < ( 40 * 1024 * 1024 ) ){
    db.foo.insert( { _id : num++ , s : bigString } );
    inserted += bigString.length;
}

db.getLastError();
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.lt( 20 , s.config.chunks.count()  , "setup2" );

function diff1(){
    var x = s.chunkCounts( "foo" );
    printjson( x )
    return Math.max( x.shard0000 , x.shard0001 ) - Math.min( x.shard0000 , x.shard0001 );
}

assert.lt( 10 , diff1() );

// Wait for balancer to kick in.
var initialDiff = diff1();
var maxRetries = 3;
while ( diff1() == initialDiff ){
    sleep( 5000 );
    assert.lt( 0, maxRetries--, "Balancer did not kick in.");
}

print("* A");
print( "disabling the balancer" );
s.config.settings.update( { _id : "balancer" }, { $set : { stopped : true } } , true );
s.config.settings.find().forEach( printjson );
print("* B");


print( diff1() )

var currDiff = diff1();
assert.repeat( function(){
    var d = diff1();
    return d != currDiff;
} , "balance with stopped flag should not have happened" , 1000 * 60 , 5000 );

s.stop()
