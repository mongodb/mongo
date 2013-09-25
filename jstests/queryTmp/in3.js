t = db.jstests_in3;

t.drop();
t.ensureIndex( {i:1} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {i:{$in:[3]}} ).itcount());
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {i:{$in:[3,6]}} ).itcount());

for ( var i=0; i<20; i++ )
    t.insert( { i : i } );

// NEW QUERY EXPLAIN
assert.eq( 2 , t.find( {i:{$in:[3,6]}} ).itcount())
