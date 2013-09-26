// Test multikey range preference for a fully included range SERVER-958.

t = db.jstests_indexq;
t.drop();

t.ensureIndex( {a:1} );
// Single key index
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:{$gt:4,$gte:5}} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:{$in:[1,2,3]},$or:[{a:{$in:[1,2]}}]} ).itcount() );

t.save( {a:[1,3]} );
// Now with multi key index.
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( {a:{$gt:4,$gte:5}} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 1, t.find( {a:{$in:[1,2,3]},$or:[{a:{$in:[1,2]}}]} ).itcount() );
