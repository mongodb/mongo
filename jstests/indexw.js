// Check that v0 keys are generated for v0 indexes SERVER-3375

t = db.jstests_indexw;
t.drop();

t.save( {a:[]} );
assert.eq( 1, t.count( {a:[]} ) );
t.ensureIndex( {a:1} );
assert.eq( 1, t.count( {a:[]} ) );
t.dropIndexes();

// The count result is incorrect - just checking here that v0 key generation is used.
t.ensureIndex( {a:1}, {v:0} );
assert.eq( 0, t.count( {a:[]} ) );
