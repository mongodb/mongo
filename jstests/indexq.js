// Test multikey range preference for a fully included range SERVER-958.

t = db.jstests_indexq;
t.drop();

t.ensureIndex( {a:1} );
// Single key index
assert.eq( 5, t.find( {a:{$gt:4,$gte:5}} ).explain().indexBounds.a[ 0 ][ 0 ] );
assert.eq( [[1,1],[2,2]], t.find( {a:{$in:[1,2,3]},$or:[{a:{$in:[1,2]}}]} ).explain().indexBounds.a );

t.save( {a:[1,3]} );
// Now with multi key index.

// QUERY_MIGRATION: If we have 2 bounds over a multikey field we can only use one and sometimes
// we pick the wrong one.

// We should know that >4 is worse than >5
// assert.eq( 5, t.find( {a:{$gt:4,$gte:5}} ).explain().indexBounds.a[ 0 ][ 0 ] );

printjson(t.find( {a:{$in:[1,2,3]},$or:[{a:{$in:[1,2]}}]} ).explain())

// We should know that in[1,2] is better than in[1,2,3].
// assert.eq( [[1,1],[2,2]], t.find( {a:{$in:[1,2,3]},$or:[{a:{$in:[1,2]}}]} ).explain().indexBounds.a );
