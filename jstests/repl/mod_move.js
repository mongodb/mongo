
// test repl basics
// data on master/slave is the same

var rt = new ReplTest( "mod_move" );

m = rt.start( true , { oplogSize : 50 } );

function block(){
    am.runCommand( { getlasterror : 1 , w : 2 , wtimeout : 3000 } )
}

am = m.getDB( "foo" );

function check( note ){
    var start = new Date();
    var x,y;
    while ( (new Date()).getTime() - start.getTime() < 5 * 60 * 1000 ){
        x = am.runCommand( "dbhash" );
        y = as.runCommand( "dbhash" );
        if ( x.md5 == y.md5 )
            return;
        sleep( 200 );
    }
    assert.eq( x.md5 , y.md5 , note );
}

// insert a lot of 'big' docs
// so when we delete them the small docs move here

BIG = 100000;
N = BIG * 2;

s : "asdasdasdasdasdasdasdadasdadasdadasdasdas"

for ( i=0; i<BIG; i++ ) {
    am.a.insert( { _id : i , s : 1 , x : 1 } )
}
for ( ; i<N; i++ ) {
    am.a.insert( { _id : i , s : 1 } )
}
for ( i=0; i<BIG; i++ ) {
    am.a.remove( { _id : i } )
}
am.getLastError();
assert.eq( BIG , am.a.count() )

assert.eq( 1 , am.a.stats().paddingFactor , "A2"  )


// start slave
s = rt.start( false );
as = s.getDB( "foo" );
for ( i=N-1; i>=BIG; i-- ) {
    am.a.update( { _id : i } , { $set : { x : 1 } } )
    if ( i == N ) {
        am.getLastError()
        assert.lt( as.a.count() , BIG , "B1" )
        print( "NOW : " + as.a.count() )
    }
}

check( "B" )

rt.stop();




