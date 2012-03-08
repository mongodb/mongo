print( "Temporary diagnostics for 32-bit Windows pushall.js failures" );
print( "db.hostInfo()" );
printjson( db.hostInfo() );
print( "db.serverStatus()" );
printjson( db.serverStatus() );
print( "db.stats()" );
printjson( db.stats() );
print( "End of temporary diagnostics for 32-bit Windows pushall.js failures" );

t = db.jstests_pushall;
t.drop();

t.save( { a: [ 1, 2, 3 ] } );
t.update( {}, { $pushAll: { a: [ 4 ] } } );
assert.eq( [ 1, 2, 3, 4 ], t.findOne().a );
t.update( {}, { $pushAll: { a: [ 4 ] } } );
assert.eq( [ 1, 2, 3, 4, 4 ], t.findOne().a );

t.drop();
t.save( { a: [ 1, 2, 3 ] } );
t.update( {}, { $pushAll: { a: [ 4, 5 ] } } );
assert.eq( [ 1, 2, 3, 4, 5 ], t.findOne().a );
t.update( {}, { $pushAll: { a: [] } } );
assert.eq( [ 1, 2, 3, 4, 5 ], t.findOne().a );

t.drop();
t.save( {} );
t.update( {}, { $pushAll: { a: [ 1, 2 ] } } );
assert.eq( [ 1, 2 ], t.findOne().a );
