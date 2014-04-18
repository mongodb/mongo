// multcollections.js

s = new ShardingTest( "multcollections" , 2 , 1 , 1 , { chunksize : 1 }  );

s.adminCommand( { enablesharding : "test" } );

db = s.getDB( "test" )

N = 100000

S = ""
while ( S.length < 500 )
    S += "123123312312";

for ( i=0; i<N; i++ ){
    db.foo.insert( { _id : i , s : S } )
    db.bar.insert( { _id : i , s : S , s2 : S } )
    db.getLastError()
}

db.printShardingStatus()

function mytest( coll , i , loopNumber ){
    x = coll.find( { _id : i } ).explain();
    if ( x )
        return;
    throw "can't find " + i + " in " + coll.getName() + " on loopNumber: " + loopNumber +  " explain: " + tojson( x );
}

loopNumber = 0
while ( 1 ){
    for ( i=0; i<N; i++ ){
        mytest( db.foo , i , loopNumber );
        mytest( db.bar , i , loopNumber );
        if ( i % 1000 == 0 )
            print( i )
    }
    db.printShardingStatus()
    loopNumber++;

    if ( loopNumber == 1 ){
        s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
        s.adminCommand( { shardcollection : "test.bar" , key : { _id : 1 } } );
    }
        
    assert( loopNumber < 1000 , "taking too long" );

    if ( s.chunkDiff( "foo" ) < 12 && s.chunkDiff( "bar" ) < 12 )
        break
}

s.stop()

