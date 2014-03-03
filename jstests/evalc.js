t = db.jstests_evalc;
t.drop();

t2 = db.evalc_done
t2.drop()

for( i = 0; i < 10; ++i ) {
    t.save( {i:i} );
}

// SERVER-1610

assert.eq( 0 , t2.count() , "X1" )

s = startParallelShell( "print( 'starting forked:' + Date() ); for ( i=0; i<50000; i++ ){ db.currentOp(); } print( 'ending forked:' + Date() ); db.evalc_done.insert( { x : 1 } ); " )

print( "starting eval: " + Date() )
while ( true ) {
    db.eval( "db.jstests_evalc.count( {i:10} );" );
    if ( t2.count() > 0 )
        break;
}
print( "end eval: " + Date() )

s();
