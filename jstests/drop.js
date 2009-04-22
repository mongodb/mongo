f = db.jstests_drop;

f.drop();

assert.eq( 0, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() );
f.save( {} );
assert.eq( 1, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() );
f.ensureIndex( {a:1} );
assert.eq( 2, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() );
assert.commandWorked( db.runCommand( {drop:"jstests_drop"} ) );
assert.eq( 0, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() );

f = db.jstests_drop;
f.ensureIndex( {a:1} );
assert.eq( 2, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() );
assert.commandWorked( db.runCommand( {deleteIndexes:"jstests_drop",index:"*"} ) );
assert.eq( 1, db.system.indexes.find( {ns:"test.jstests_drop"} ).count() );

// make sure we can still use it
f.save( {} );
assert.eq( 1, f.find().hint( {_id:ObjectId( "000000000000000000000000" )} ).toArray().length );
