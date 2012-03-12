// sharding_balance4.js

// check that doing updates done during a migrate all go to the right place

s = new ShardingTest( "slow_sharding_balance4" , 2 , 1 , 1 , { chunksize : 1 } )

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

for ( var i=0; i<50; i++ ){
    s.printChunks( "test.foo" )
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
        
        if ( dontAssert ){
            if ( z )
                delete z.s;
            print( "not asserting for key failure: " + x + " want: " + e + " got: " + tojson(z) )
            return false;
        }

        // we will assert past this point but wait a bit to see if it is because the missing update
        // was being held in the writeback roundtrip
        sleep( 10000 );
        
        var y = db.foo.findOne( { _id : parseInt( x ) } )

        if ( y ){
            delete y.s;
        }

        s.printChunks( "test.foo" )
        
        assert( z , "couldn't find : " + x + " y:" + tojson(y) + " e: " + e + " " + msg )
        assert.eq( e , z.x , "count for : " + x + " y:" + tojson(y) + " " + msg )
    }

    return true;
}

function diff1(){
    var myid = doUpdate( false )
    var le = db.getLastErrorCmd();

    if ( le.err )
        print( "ELIOT ELIOT : " + tojson( le ) + "\t" + myid );

    if ( ! le.updatedExisting || le.n != 1 ) {
        print( "going to assert for id: " + myid + " correct count is: " + counts[myid] + " db says count is: " + tojson(db.foo.findOne( { _id : myid } )) );
    }

    assert( le.updatedExisting , "GLE diff myid: " + myid + " 1: " + tojson(le) )
    assert.eq( 1 , le.n , "GLE diff myid: " + myid + " 2: " + tojson(le) )


    if ( Math.random() > .99 ){
        db.getLastError()
        check( "random late check" ); // SERVER-1430 
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

assert.lt( 20 , diff1() ,"initial load" );
print( diff1() )

assert.soon( function(){
    
    var d = diff1();
    return d < 5;
} , "balance didn't happen" , 1000 * 60 * 3 , 1 );
    

s.stop();
