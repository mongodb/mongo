// jump1.js

s = new ShardingTest( "jump1" , 2 /* numShards */, 2 /* verboseLevel */, 1 /* numMongos */, { chunksize : 1 } )

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { x : 1 } } );

db = s.getDB( "test" );

sh.setBalancerState( false )

big = ""
while ( big.length < 10000 )
    big += "."

x = 0;
for ( ; x < 500; x++ )
    db.foo.insert( { x : x , big : big } )

for ( i=0; i<500; i++ )
    db.foo.insert( { x : x , big : big } )

for ( ; x < 2000; x++ )
    db.foo.insert( { x : x , big : big } )


db.getLastError();

sh.status(true)

res = sh.moveChunk( "test.foo" , { x : 0 } , "shard0001" )
if ( ! res.ok )
    res = sh.moveChunk( "test.foo" , { x : 0 } , "shard0000" )

sh.status(true)

sh.setBalancerState( true )

function diff1(){
    var x = s.chunkCounts( "foo" );
    printjson( x )
    return Math.max( x.shard0000 , x.shard0001 ) - Math.min( x.shard0000 , x.shard0001 );
}

assert.soon( function(){
    var d = diff1();
    print( "diff: " + d );
    sh.status(true)
    return d < 5;
} , "balance didn't happen" , 1000 * 60 * 5 , 5000 );


s.stop()
