// Test update modifier uassert during initial sync. SERVER-4781

rt = new ReplTest( "repl13tests" );

m = rt.start( true );
mc = m.getDB( 'd' )[ 'c' ];

for( i = 0; i < 100000; ++i ) {
    mc.save( {_id:i,a:{}} );
}
m.getDB( 'd' ).getLastError();

s = rt.start( false );
sc = s.getDB( 'd' )[ 'c' ];

assert.soon( function() { printjson( sc.count() ); return sc.count() > 0; } );

for( i = 99999; i >= 90000; --i ) {
    // If the document is cloned as {a:1}, the {$set:{'a.b':1}} modifier will uassert.
    mc.update( {_id:i}, {$set:{'a.b':1}} );
    mc.update( {_id:i}, {$set:{a:1}} );    
}

mc.save( {} )
assert.soon( function() { return sc.count() == 100001; } );

printjson( sc.findOne( {_id:99999} ) );
printjson( sc.findOne( {_id:90000} ) );
