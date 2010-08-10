t = db.jstests_evalc;
t.drop();

for( i = 0; i < 10; ++i ) {
    t.save( {i:i} );
}

// SERVER-1610
if ( 0 ) {

function op() {
    uri = db.runCommand( "whatsmyuri" ).you;
    printjson( uri );
    p = db.currentOp().inprog;
    for ( var i in p ) {
        var o = p[ i ];
        if ( o.client == uri ) {
            return o.opid;
        }
    }
    return -1;
}

s = startParallelShell( "db.eval( 'while( 1 ) { db.jstests_evalc.count( {i:10} ); }' )" );

assert.soon( "op() != -1" );
//db.killOp( op() );

s();

}