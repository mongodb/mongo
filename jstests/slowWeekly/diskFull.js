/*
x = 1

t = db.diskFullUpdates;
t.drop();

assert.eq( 0 , t.count() );

s = ""
while ( s.length < 1000 )
    s += ".";

N = 0;
while ( true ) {
    t.insert( { _id : N++ , x : [] , s : s } );
    err = db.getLastError();
    if ( err ) {
        assert.eq("Can't take a write lock while out of disk space", err);
        print( err );
        break;
    }

    if ( N % 100 ) {
        printjson( t.stats() );
    }
}

printjson( t.stats() );

assert.eq(N-1, t.count());
N = t.count();

print( "Num documents created: " + N );

numAttempts = 0
while ( numAttempts < 20 ) {
    for ( i=0; i<N; i++ ) {
        t.update( { _id : i } , { $push : { x : 1 } } );
        print( db.getLastError() );
    }
    assert.eq( N , t.count() );
    numAttempts++
}

*/
