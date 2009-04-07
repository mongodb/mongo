f = db.jstests_drop;

f.drop();

f.ensureIndex( {a:1} );
assert.eq( 1, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() );
assert.commandWorked( db.runCommand( {drop:"jstests_drop"} ) );
assert.eq( 0, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() );
