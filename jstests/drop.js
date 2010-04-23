f = db.jstests_drop;

f.drop();

assert.eq( 0, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() , "A" );
f.save( {} );
assert.eq( 1, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() , "B" );
f.ensureIndex( {a:1} );
assert.eq( 2, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() , "C" );
assert.commandWorked( db.runCommand( {drop:"jstests_drop"} ) );
assert.eq( 0, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() , "D" );

f.resetIndexCache();
f.ensureIndex( {a:1} );
assert.eq( 2, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() , "E" );
assert.commandWorked( db.runCommand( {deleteIndexes:"jstests_drop",index:"*"} ), "delete indexes A" );
assert.eq( 1, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() , "G" );

// make sure we can still use it
f.save( {} );
assert.eq( 1, f.find().hint( "_id_" ).toArray().length , "H" );
