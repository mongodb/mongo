t = db.jstests_evalc;
t.drop();

for( i = 0; i < 10; ++i ) {
    t.save( {i:i} );
}

// SERVER-1610

function op() {
    uri = db.runCommand( "whatsmyuri" ).you;
    printjson( uri );
    p = db.currentOp().inprog;
    for ( var i in p ) {
        var o = p[ i ];
        if ( o.client == uri ) {
            print( "found it" );
            return o.opid;
        }
    }
    return -1;
}

s = startParallelShell( "print( 'starting forked:' + Date() ); for ( i=0; i<500000; i++ ){ db.currentOp(); } print( 'ending forked:' + Date() ); " )

print( "starting eval: " + Date() )
for ( i=0; i<50000; i++ ){
    db.eval( "db.jstests_evalc.count( {i:10} );" );
}
print( "end eval: " + Date() )

s();
