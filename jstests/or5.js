t = db.jstests_or5;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:2},{b:3},{}]} ).explain().cursor" );
assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:2},{b:3},{c:4}]} ).explain().cursor" );
assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:2},{b:3}]} ).sort( {c:1} ).explain().cursor" );
e = t.find( {$or:[{a:2},{b:3}]} ).sort( {a:1} ).explain();
assert.eq.automsg( "'BtreeCursor a_1'", "e.cursor" );
assert.eq.automsg( "1", "e.indexBounds[ 0 ][ 0 ].a.$minElement" );
assert.eq.automsg( "1", "e.indexBounds[ 0 ][ 1 ].a.$maxElement" );

t.ensureIndex( {z:"2d"} );

assert.eq.automsg( "'GeoSearchCursor'", "t.find( {z:{$near:[50,50]},a:2} ).explain().cursor" );
assert.eq.automsg( "'GeoSearchCursor'", "t.find( {z:{$near:[50,50]},$or:[{a:2}]} ).explain().cursor" );
assert.eq.automsg( "'GeoSearchCursor'", "t.find( {$or:[{a:2}],z:{$near:[50,50]}} ).explain().cursor" );
assert.eq.automsg( "'GeoSearchCursor'", "t.find( {$or:[{a:2},{b:3}],z:{$near:[50,50]}} ).explain().cursor" );
assert.throws.automsg( function() { return t.find( {$or:[{z:{$near:[50,50]}},{a:2}]} ).toArray(); } );
