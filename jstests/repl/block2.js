
var rt = new ReplTest( "block1" );

m = rt.start( true );
s = rt.start( false );

function setup(){
    
    dbm = m.getDB( "foo" );
    dbs = s.getDB( "foo" );
    
    tm = dbm.bar;
    ts = dbs.bar;
}
setup();

function check( msg ){
    assert.eq( tm.count() , ts.count() , "check: " + msg );
}

check( "A" );

tm.save( { x : 1 } );
dbm.getLastError( 2 );
check( "B" );

tm.save( { x : 2 } );
dbm.getLastError( 2 , 500 );
check( "C" );

rt.stop( false );
tm.save( { x : 3 } )
assert.eq( 3 , tm.count() , "D1" );
assert.throws( function(){ dbm.getLastError( 2 , 500 ); } , "D2" )

s = rt.start( false )
setup();
dbm.getLastError( 2 , 30000 )
check( "D3" )

rt.stop();




