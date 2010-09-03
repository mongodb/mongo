// sharding_balance4.js

s = new ShardingTest( "slow_sharding_balance4" , 2 , 2 , 1 , { chunksize : 1 } )

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.eq( 1 , s.config.chunks.count()  , "setup1" );

s.config.settings.find().forEach( printjson )

db = s.getDB( "test" );

bigString = ""
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

N = 3000

num = 0;

counts = {}

function doUpdate( includeString ){
    var up = { $inc : { x : 1 } }
    if ( includeString )
        up["$set"] = { s : bigString };
    var myid = Random.randInt( N )
    db.foo.update( { _id : myid } , up , true );

    counts[myid] = ( counts[myid] ? counts[myid] : 0 ) + 1;
    return myid;
}

for ( i=0; i<N*10; i++ ){
    doUpdate( true )
}
db.getLastError();

s.printChunks( "test.foo" )

for ( var i=0; i<10; i++ ){
    if ( check( "initial:" + i , true ) )
        break;
    sleep( 5000 )
}
check( "initial at end" )


assert.lt( 20 , s.config.chunks.count()  , "setup2" );

function check( msg , dontAssert ){
    for ( var x in counts ){
        var e = counts[x];
        var z = db.foo.findOne( { _id : parseInt( x ) } )
        
        if ( z && z.x == e )
            continue;
        
        if ( dontAssert )
            return false;

        sleep( 10000 );
        
        var y = db.foo.findOne( { _id : parseInt( x ) } )

        if ( y ){
            delete y.s;
        }
        
        assert( z , "couldn't find : " + x + " y:" + tojson(y) + " e: " + e + " " + msg )
        assert.eq( e , z.x , "count for : " + x + " y:" + tojson(y) + " " + msg )
    }

    return true;
}

function diff(){
    var myid = doUpdate( false )
    var le = db.getLastErrorCmd();
    if ( le.err )
        print( "ELIOT ELIOT : " + tojson( le ) + "\t" + myid );

    if ( Math.random() > .99 ){
        db.getLastError()
        check(); // SERVER-1430  TODO
    }

    var x = s.chunkCounts( "foo" )
    if ( Math.random() > .999 )
        printjson( x )
    return Math.max( x.shard0000 , x.shard0001 ) - Math.min( x.shard0000 , x.shard0001 );
}

function sum(){
    var x = s.chunkCounts( "foo" )
    return x.shard0000 + x.shard0001;
}

assert.lt( 20 , diff() ,"initial load" );
print( diff() )

assert.soon( function(){
    
    var d = diff();
    return d < 5;
} , "balance didn't happen" , 1000 * 60 * 3 , 1 );
    

s.stop();
