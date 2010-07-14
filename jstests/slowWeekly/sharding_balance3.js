// sharding_balance3.js

s = new ShardingTest( "slow_sharding_balance3" , 2 , 2 , 1 , { chunksize : 1 } );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.eq( 1 , s.config.chunks.count()  , "setup1" );

s.config.settings.find().forEach( printjson );

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
assert.lt( 20 , s.config.chunks.count()  , "setup2" );

function dist(){
    var x = {}
    s.config.chunks.find( { ns : "test.foo" } ).forEach(
        function(z){
            if ( x[z.shard] )
                x[z.shard]++
            else
                x[z.shard] = 1;
        }
    );
    return x;
}

function diff(){
    var x = dist();
    printjson( x )
    return Math.max( x.shard0 , x.shard1 ) - Math.min( x.shard0 , x.shard1 );
}

assert.lt( 20 , diff() );

// Wait for balancer to kick in.
var initialDiff = diff();
var maxRetries = 3;
while ( diff() == initialDiff ){
    sleep( 5000 );
    assert.lt( 0, maxRetries--, "Balancer did not kick in.");
}

print("* A");
print( "disabling the balancer" );
s.config.settings.update( { _id : "balancer" }, { $set : { stopped : true } } );
s.config.settings.find().forEach( printjson );
print("* B");


print( diff() )

var currDiff = diff();
assert.repeat( function(){
    var d = diff();
    return d != currDiff;
} , "balance with stopped flag should not have happened" , 1000 * 30 , 5000 );

s.stop()