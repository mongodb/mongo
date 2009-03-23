db.f.drop();
db.f.save( {} );
db.f.save( {} );
db.f.save( {} );

db.getMongo().getDB( "admin" ).runCommand( {closeAllDatabases:1} );

assert.eq( 0, db.runCommand( {cursorInfo:1} ).clientCursors_size );
assert.eq( 2, db.f.find( {} ).limit( 2 ).toArray().length );
assert.eq( 1, db.runCommand( {cursorInfo:1} ).clientCursors_size );
