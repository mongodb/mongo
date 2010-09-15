t = db.jstests_evald;
t.drop();

// only run in spidermonkey, not in v8 - see SERVER-387
if ( typeof _threadInject == "undefined" ){

function debug( x ) {
//    print( x );
}

for( i = 0; i < 10; ++i ) {
    t.save( {i:i} );
}

ev = "while( 1 ) { db.jstests_evald.count( {i:10} ); }"

function op() {
    p = db.currentOp().inprog;
    for ( var i in p ) {
        var o = p[ i ];
        if ( o.active && o.query && o.query.$eval && o.query.$eval == ev ) {
            return o.opid;
        }
    }
    return -1;
}

s = startParallelShell( "db.eval( '" + ev + "' )" );

o = null;
assert.soon( function() { o = op(); return o != -1 } );

debug( "going to kill" );

db.killOp( o );

debug( "sent kill" );

s();

}