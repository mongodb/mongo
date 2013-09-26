// Test multikey range preference for a fully included range SERVER-958.

t = db.jstests_indexq;
t.drop();

t.ensureIndex( {a:1} );
// Single key index
assert.eq( 5, t.find( {a:{$gt:4,$gte:5}} ).explain().indexBounds.a[ 0 ][ 0 ] );
assert.eq( [[1,1],[2,2]], t.find( {a:{$in:[1,2,3]},$or:[{a:{$in:[1,2]}}]} ).explain().indexBounds.a );

t.save( {a:[1,3]} );
// Now with multi key index.
assert.eq( 5, t.find( {a:{$gt:4,$gte:5}} ).explain().indexBounds.a[ 0 ][ 0 ] );
assert.eq( [[1,1],[2,2]], t.find( {a:{$in:[1,2,3]},$or:[{a:{$in:[1,2]}}]} ).explain().indexBounds.a );
