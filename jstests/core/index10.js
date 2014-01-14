// unique index, drop dups

t = db.jstests_index10;
t.drop();

t.save( {i:1} );
t.save( {i:2} );
t.save( {i:1} );
t.save( {i:3} );
t.save( {i:1} );

t.ensureIndex( {i:1} );
assert.eq( 5, t.count() );
t.dropIndexes();
var err = t.ensureIndex( {i:1}, true );
assert.writeError(err)
assert.eq( 11000, err.getWriteError().code );

assert( 1 == db.system.indexes.count( {ns:"test.jstests_index10" } ), "only id index" );
// t.dropIndexes();

ts = t.totalIndexSize();
t.ensureIndex( {i:1}, [ true, true ] );
ts2 = t.totalIndexSize();

assert.eq( ts * 2, ts2, "totalIndexSize fail" );

assert.eq( 3, t.count() );
assert.eq( 1, t.count( {i:1} ) );

t.ensureIndex( {j:1}, [ true, true ] );
assert.eq( 1, t.count() );
