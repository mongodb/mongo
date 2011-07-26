t = db.jstests_evald;
t.drop();

function debug( x ) {
//    printjson( x );
}

for( i = 0; i < 10; ++i ) {
    t.save( {i:i} );
}
db.getLastError();

function op( ev, where ) {
    p = db.currentOp().inprog;
    debug( p );
    for ( var i in p ) {
        var o = p[ i ];
        if ( where ) {
            if ( o.active && o.query && o.query.query && o.query.query.$where && o.ns == "test.jstests_evald" ) {
                return o.opid;
            }
        } else {
            if ( o.active && o.query && o.query.$eval && o.query.$eval == ev ) {
                return o.opid;
            }
        }
    }
    return -1;
}

function doIt( ev, wait, where ) {

    if ( where ) {
        s = startParallelShell( ev );
    } else {
        s = startParallelShell( "db.eval( '" + ev + "' )" );        
    }

    o = null;
    assert.soon( function() { o = op( ev, where ); return o != -1 } );

    if ( wait ) {
        sleep( 2000 );
    }

    debug( "going to kill" );

    db.killOp( o );

    debug( "sent kill" );

    s();

}

doIt( "db.jstests_evald.count( { $where: function() { while( 1 ) { sleep(1); } } } )", true, true );
doIt( "db.jstests_evald.count( { $where: function() { while( 1 ) { sleep(1); } } } )", false, true );
doIt( "while( true ) { sleep(1);}", false );
doIt( "while( true ) { sleep(1);}", true );

// the for loops are currently required, as a spawned op masks the parent op - see SERVER-1931
doIt( "while( 1 ) { for( var i = 0; i < 10000; ++i ) {;} db.jstests_evald.count( {i:10} ); }", true );
doIt( "while( 1 ) { for( var i = 0; i < 10000; ++i ) {;} db.jstests_evald.count( {i:10} ); }", false );
doIt( "while( 1 ) { for( var i = 0; i < 10000; ++i ) {;} db.jstests_evald.count(); }", true );
doIt( "while( 1 ) { for( var i = 0; i < 10000; ++i ) {;} db.jstests_evald.count(); }", false );

doIt( "while( 1 ) { for( var i = 0; i < 10000; ++i ) {;} try { db.jstests_evald.count( {i:10} ); } catch ( e ) { } }", true );
doIt( "while( 1 ) { try { while( 1 ) { sleep(1); } } catch ( e ) { } }", true );
