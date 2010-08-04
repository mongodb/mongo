// tests sharding with replica sets

s = new ShardingTest( "rs1" , 3 , 1 , 2 , { rs : true , chunksize : 1 } )

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
    var total = 0;
    var min = 1000000000;
    var max = 0;
    for ( var sn in x ){
        total += x[sn];
        if ( x[sn] < min )
            min = x[sn];
        if ( x[sn] > max )
            max = x[sn];
    }
    
    print( tojson(x) + " total: " + total + " min: "  + min +  " max: " + max )
    return max - min;
}

assert.lt( 20 , diff() , "big differential here" );
print( diff() )

assert.soon( function(){
    var d = diff();
    return d < 5;
} , "balance didn't happen" , 1000 * 60 * 3 , 5000 );


for ( i=0; i<s._rs.length; i++ ){
    r = s._rs[i];
    r.test.awaitReplication();
    x = r.test.getHashes( "test" );
    print( r.url + "\t" + tojson( x ) )
    for ( j=0; j<x.slaves.length; j++ )
        assert.eq( x.master.md5 , x.slaves[j].md5 , "hashes same for: " + r.url + " slave: " + j );
}

s.stop()
