// test listIndexes command

t = db.list_indexes1;
t.drop();

res = t.runCommand( "listIndexes" );
assert.commandFailed(res);

t.insert( { x : 1 } );

res = t.runCommand( "listIndexes" );
assert.eq( 1, res.indexes.length, tojson( res ) );

t.ensureIndex( { x : 1 } );

res = t.runCommand( "listIndexes" );
assert.eq( 2, res.indexes.length, tojson( res ) );
