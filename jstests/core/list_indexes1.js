// test listIndexes command

t = db.list_indexes1;
t.drop();

res = t.runCommand( "listIndexes", { cursor: {} } );
assert.commandFailed(res);

t.insert( { x : 1 } );

res = t.runCommand( "listIndexes", { cursor: {} } );
indexes = new DBCommandCursor( db.getMongo(), res ).toArray();
assert.eq( 1, indexes.length, tojson( res ) );

t.ensureIndex( { x : 1 } );

res = t.runCommand( "listIndexes", { cursor: {} } );
indexes = new DBCommandCursor( db.getMongo(), res ).toArray();
assert.eq( 2, indexes.length, tojson( res ) );
