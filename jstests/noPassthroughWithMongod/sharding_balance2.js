// sharding_balance2.js

s = new ShardingTest( "slow_sharding_balance2" , 2 , 1 , 1 , { chunksize : 1 , manualAddShard : true } )

names = s.getConnNames();
for ( var i=0; i<names.length; i++ ){
    if ( i==1 ) {
        // We set maxSize of the shard to something artificially low. That mongod would still 
        // allocate and mmap storage as usual but the balancing mongos would not ship any chunk
        // to it.
        s.adminCommand( { addshard : names[i] , maxSize : 1 } );
    } else {
        s.adminCommand( { addshard : names[i] } );
    }
}

s.adminCommand( { enablesharding : "test" } );

s.config.settings.find().forEach( printjson )

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
print( diff1() )

var currDiff = diff1();
assert.repeat( function(){
    var d = diff1();
    return d != currDiff;
} , "balance with maxSize should not have happened" , 1000 * 30 , 5000 );
    

s.stop();
