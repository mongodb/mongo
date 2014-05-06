// tests sharding with replica sets

s = new ShardingTest( "rs1" , 3 /* numShards */, 1 /* verboseLevel */, 2 /* numMongos */, { rs : true , chunksize : 1 } )

s.adminCommand( { enablesharding : "test" } );
s.config.settings.update( { _id: "balancer" }, { $set : { _waitForDelete : true } } , true );

s.config.settings.find().forEach( printjson )

db = s.getDB( "test" );

bigString = ""
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

inserted = 0;
num = 0;
while ( inserted < ( 20 * 1024 * 1024 ) ){
    db.foo.insert( { _id : num++ , s : bigString , x : Math.random() } );
    inserted += bigString.length;
}

db.getLastError();
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.lt( 20 , s.config.chunks.count()  , "setup2" );

function diff1(){
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

assert.lt( 20 , diff1() , "big differential here" );
print( diff1() )

{
    // quick test for SERVER-2686
    var mydbs = db.getMongo().getDBs().databases;
    for ( var i=0; i<mydbs.length; i++ ) {
        assert( mydbs[i].name != "local" , "mongos listDatabases can't return local" );
    }
}


assert.soon( function(){
    var d = diff1();
    return d < 5;
} , "balance didn't happen" , 1000 * 60 * 6 , 5000 );

jsTest.log("Stopping balancer");
s.stopBalancer();

jsTest.log("Balancer stopped, checking dbhashes");
for ( i=0; i<s._rs.length; i++ ){
    r = s._rs[i];
    r.test.awaitReplication();
    x = r.test.getHashes( "test" );
    print( r.url + "\t" + tojson( x ) )
    for ( j=0; j<x.slaves.length; j++ )
        assert.eq( x.master.md5 , x.slaves[j].md5 , "hashes not same for: " + r.url + " slave: " + j );
}


assert.eq( num , db.foo.find().count() , "C1" )
assert.eq( num , db.foo.find().itcount() , "C2" )
assert.eq( num , db.foo.find().sort( { _id : 1 } ).itcount() , "C3" )
assert.eq( num , db.foo.find().sort( { _id : -1 } ).itcount() , "C4" )
db.foo.ensureIndex( { x : 1 } )
assert.eq( num , db.foo.find().sort( { x : 1 } ).itcount() , "C5" )
assert.eq( num , db.foo.find().sort( { x : -1 } ).itcount() , "C6" )


s.stop()
