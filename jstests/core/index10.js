// unique index, drop dups

t = db.jstests_index10;
t.drop();

t.save( {_id:1, i:1} );
t.save( {_id:2, i:2} );
t.save( {_id:3, i:1} );
t.save( {_id:4, i:3} );
t.save( {_id:5, i:1} );

t.ensureIndex( {i:1} );
assert.eq( 5, t.count() );
t.dropIndexes();
var err = t.ensureIndex( {i:1}, true );
assert.commandFailed(err)
assert.eq( 11000, err.code );

assert( 1 == t.getIndexes().length, "only id index" );
// t.dropIndexes();

t.ensureIndex( {i:1}, [ true, true ] );

assert.eq( 3, t.count() );
assert.eq( 1, t.count( {i:1} ) );

stats = t.stats();
assert.eq( stats.indexSizes["_id_"],
           stats.indexSizes["i_1"] );

t.ensureIndex( {j:1}, [ true, true ] );
assert.eq( 1, t.count() );
