t = db.jstests_evalc;
t.drop();

for( i = 0; i < 10; ++i ) {
    t.save( {i:i} );
}

// SERVER-1610

s = startParallelShell( "print( 'starting forked:' + Date() ); for ( i=0; i<500000; i++ ){ db.currentOp(); } print( 'ending forked:' + Date() ); " )

print( "starting eval: " + Date() )
for ( i=0; i<20000; i++ ){
    db.eval( "db.jstests_evalc.count( {i:10} );" );
}
print( "end eval: " + Date() )

s();
